/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gsi_service.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android/gsi/IGsiService.h>
#include <fs_mgr_dm_linear.h>
#include <libfiemap_writer/fiemap_writer.h>
#include <logwrap/logwrap.h>
#include "file_paths.h"

namespace android {
namespace gsi {

using namespace std::literals;
using namespace android::fs_mgr;
using android::fiemap_writer::FiemapWriter;

static constexpr char kUserdataDevice[] = "/dev/block/by-name/userdata";

// The default size of userdata.img for GSI.
// We are looking for /data to have atleast 40% free space
static constexpr uint32_t kMinimumFreeSpaceThreshold = 40;
// We determine the fragmentation by making sure the files
// we create don't have more than 16 extents.
static constexpr uint32_t kMaximumExtents = 512;
static constexpr std::chrono::milliseconds kDmTimeout = 5000ms;

void GsiService::Register() {
    auto ret = android::BinderService<GsiService>::publish();
    if (ret != android::OK) {
        LOG(FATAL) << "Could not register gsi service: " << ret;
    }
}

GsiService::~GsiService() {
    PostInstallCleanup();
}

binder::Status GsiService::startGsiInstall(int64_t gsiSize, int64_t userdataSize,
                                           bool* _aidl_return) {
    std::lock_guard<std::mutex> guard(main_lock_);

    // Make sure any interrupted installations are cleaned up.
    PostInstallCleanup();

    if (!StartInstall(gsiSize, userdataSize)) {
        // Perform local cleanup and delete any lingering files.
        PostInstallCleanup();
        RemoveGsiInstall();
        *_aidl_return = false;
    } else {
        *_aidl_return = true;
    }
    return binder::Status::ok();
}

binder::Status GsiService::commitGsiChunkFromStream(const android::os::ParcelFileDescriptor& stream,
                                                    int64_t bytes, bool* _aidl_return) {
    std::lock_guard<std::mutex> guard(main_lock_);

    *_aidl_return = CommitGsiChunk(stream.get(), bytes);
    return binder::Status::ok();
}

binder::Status GsiService::commitGsiChunkFromMemory(const std::vector<uint8_t>& bytes,
                                                    bool* _aidl_return) {
    std::lock_guard<std::mutex> guard(main_lock_);

    *_aidl_return = CommitGsiChunk(bytes.data(), bytes.size());
    return binder::Status::ok();
}

binder::Status GsiService::setGsiBootable(bool* _aidl_return) {
    std::lock_guard<std::mutex> guard(main_lock_);

    *_aidl_return = SetGsiBootable();
    return binder::Status::ok();
}

binder::Status GsiService::removeGsiInstall(bool* _aidl_return) {
    std::lock_guard<std::mutex> guard(main_lock_);

    // Just in case an install was left hanging.
    PostInstallCleanup();

    *_aidl_return = RemoveGsiInstall();
    return binder::Status::ok();
}

binder::Status GsiService::isGsiRunning(bool* _aidl_return) {
    *_aidl_return = IsGsiRunning();
    return binder::Status::ok();
}

void GsiService::PostInstallCleanup() {
    // This must be closed before unmapping partitions.
    system_fd_ = {};

    DestroyLogicalPartition("userdata_gsi", kDmTimeout);
    DestroyLogicalPartition("system_gsi", kDmTimeout);

    installing_ = false;
}

bool GsiService::StartInstall(int64_t gsi_size, int64_t userdata_size) {
    gsi_size_ = gsi_size;
    userdata_size_ = userdata_size;

    if (!PerformSanityChecks() || !PreallocateFiles() || !FormatUserdata()) {
        return false;
    }

    // Map system_gsi so we can write to it.
    std::string block_device;
    if (!CreateLogicalPartition(kUserdataDevice, *metadata_.get(), "system_gsi", true, kDmTimeout,
                                &block_device)) {
        LOG(ERROR) << "Error creating device-mapper node for system_gsi";
        return false;
    }

    static const int kOpenFlags = O_RDWR | O_NOFOLLOW | O_CLOEXEC;
    system_fd_.reset(open(block_device.c_str(), kOpenFlags));
    if (system_fd_ < 0) {
        PLOG(ERROR) << "could not open " << block_device;
        return false;
    }

    installing_ = true;
    return true;
}

bool GsiService::PerformSanityChecks() {
    if (gsi_size_ < 0) {
        LOG(ERROR) << "image size " << gsi_size_ << " is negative";
        return false;
    }

    if (!EnsureFolderExists(kGsiDataFolder) || !EnsureFolderExists(kGsiMetadataFolder)) {
        return false;
    }

    struct statvfs sb;
    if (statvfs(kGsiDataFolder, &sb)) {
        PLOG(ERROR) << "failed to read file system stats";
        return false;
    }

    // This is the same as android::vold::GetFreebytes() but we also
    // need the total file system size so we open code it here.
    uint64_t free_space = sb.f_bavail * sb.f_frsize;
    uint64_t fs_size = sb.f_blocks * sb.f_frsize;
    if (free_space <= (gsi_size_ + userdata_size_)) {
        LOG(ERROR) << "not enough free space (only" << free_space << " bytes available)";
        return false;
    }
    // We are asking for 40% of the /data to be empty.
    // TODO: may be not hard code it like this
    double free_space_percent = ((1.0 * free_space) / fs_size) * 100;
    if (free_space_percent < kMinimumFreeSpaceThreshold) {
        LOG(ERROR) << "free space " << static_cast<uint64_t>(free_space_percent)
                   << "% is below the minimum threshold of " << kMinimumFreeSpaceThreshold << "%";
        return false;
    }
    return true;
}

bool GsiService::PreallocateFiles() {
    // Create fallocated files.
    auto userdata_image = CreateFiemapWriter(kUserdataFile, userdata_size_);
    if (!userdata_image) {
        return false;
    }
    auto system_image = CreateFiemapWriter(kSystemFile, gsi_size_);
    if (!system_image) {
        return false;
    }

    // Save the extent information in liblp.
    auto builder = CreateMetadataBuilder();
    if (!builder) {
        return false;
    }

    Partition* userdata = builder->AddPartition("userdata_gsi", LP_PARTITION_ATTR_NONE);
    Partition* system = builder->AddPartition("system_gsi", LP_PARTITION_ATTR_READONLY);
    if (!userdata || !system) {
        LOG(ERROR) << "Error creating partition table";
        return false;
    }
    if (!AddPartitionFiemap(builder.get(), userdata, userdata_image.get()) ||
        !AddPartitionFiemap(builder.get(), system, system_image.get())) {
        return false;
    }

    metadata_ = builder->Export();
    if (!metadata_) {
        LOG(ERROR) << "Error exporting partition table";
        return false;
    }

    system_image->Flush();
    userdata_image->Flush();

    // We're ready to start streaming data in.
    gsi_bytes_written_ = 0;
    userdata_block_size_ = userdata_image->block_size();
    system_block_size_ = system_image->block_size();
    return true;
}

fiemap_writer::FiemapUniquePtr GsiService::CreateFiemapWriter(const std::string& path,
                                                              uint64_t size) {
    auto file = FiemapWriter::Open(path, size);
    if (!file) {
        LOG(ERROR) << "failed to create " << path;
        return nullptr;
    }

    uint64_t extents = file->extents().size();
    if (extents > kMaximumExtents) {
        LOG(ERROR) << "file " << path << " has too many extents: " << extents;
        return nullptr;
    }
    return file;
}

bool GsiService::CommitGsiChunk(int stream_fd, int64_t bytes) {
    if (bytes < 0) {
        LOG(ERROR) << "chunk size " << bytes << " is negative";
        return false;
    }

    auto buffer = std::make_unique<char[]>(system_block_size_);

    uint64_t remaining = bytes;
    while (remaining) {
        // :TODO: check file pin status!
        size_t max_to_read = std::min(system_block_size_, remaining);
        ssize_t rv = TEMP_FAILURE_RETRY(read(stream_fd, buffer.get(), max_to_read));
        if (rv < 0) {
            PLOG(ERROR) << "read gsi chunk";
            return false;
        }
        if (rv == 0) {
            LOG(ERROR) << "no bytes left in stream";
            return false;
        }
        if (!CommitGsiChunk(buffer.get(), rv)) {
            return false;
        }
        CHECK(static_cast<uint64_t>(rv) <= remaining);
        remaining -= rv;
    }
    return true;
}

bool GsiService::CommitGsiChunk(const void* data, size_t bytes) {
    if (!installing_) {
        LOG(ERROR) << "no gsi installation in progress";
        return false;
    }
    if (static_cast<uint64_t>(bytes) > gsi_size_ - gsi_bytes_written_) {
        // We cannot write past the end of the image file.
        LOG(ERROR) << "chunk size " << bytes << " exceeds remaining image size (" << gsi_size_
                   << " expected, " << gsi_bytes_written_ << " written)";
        return false;
    }
    if (!android::base::WriteFully(system_fd_, data, bytes)) {
        PLOG(ERROR) << "write failed";
        return false;
    }
    gsi_bytes_written_ += bytes;
    return true;
}

bool GsiService::SetGsiBootable() {
    if (!installing_) {
        LOG(ERROR) << "no gsi installation in progress";
        return false;
    }
    if (gsi_bytes_written_ != gsi_size_) {
        // We cannot boot if the image is incomplete.
        LOG(ERROR) << "image incomplete; expected " << gsi_size_ << " bytes, waiting for "
                   << (gsi_size_ - gsi_bytes_written_) << " bytes";
        return false;
    }

    if (fsync(system_fd_)) {
        PLOG(ERROR) << "fsync failed";
        return false;
    }

    if (!CreateMetadataFile() || !CreateBootableFile()) {
        return false;
    }

    PostInstallCleanup();
    return true;
}

bool GsiService::RemoveGsiInstall() {
    const std::vector<std::string> files{
            kUserdataFile,
            kSystemFile,
            kGsiBootableFile,
            kGsiLpMetadataFile,
    };
    bool ok = true;
    for (const auto& file : files) {
        std::string message;
        if (!android::base::RemoveFileIfExists(file, &message)) {
            LOG(ERROR) << message;
            ok = false;
        }
    }
    installing_ = false;
    return ok;
}

std::unique_ptr<android::fs_mgr::MetadataBuilder> GsiService::CreateMetadataBuilder() {
    PartitionOpener opener;
    BlockDeviceInfo userdata_device;
    if (!opener.GetInfo("userdata", &userdata_device)) {
        LOG(ERROR) << "Error reading userdata partition";
        return nullptr;
    }

    std::vector<BlockDeviceInfo> block_devices = {userdata_device};
    auto builder = MetadataBuilder::New(block_devices, "userdata", 128 * 1024, 1);
    if (!builder) {
        LOG(ERROR) << "Error creating metadata builder";
        return nullptr;
    }
    builder->IgnoreSlotSuffixing();
    return builder;
}

bool GsiService::CreateMetadataFile() {
    if (!WriteToImageFile(kGsiLpMetadataFile, *metadata_.get())) {
        LOG(ERROR) << "Error writing GSI partition table image";
        return false;
    }
    return true;
}

bool GsiService::FormatUserdata() {
    std::string block_device;
    if (!CreateLogicalPartition(kUserdataDevice, *metadata_.get(), "userdata_gsi", true, kDmTimeout,
                                &block_device)) {
        LOG(ERROR) << "Error creating device-mapper node for userdata_gsi";
        return false;
    }

    std::string block_size = std::to_string(userdata_block_size_);
    const char* const mke2fs_args[] = {
            "/system/bin/mke2fs", "-t",    "ext4", "-b", block_size.c_str(),
            block_device.c_str(), nullptr,
    };
    int rc = android_fork_execvp(arraysize(mke2fs_args), const_cast<char**>(mke2fs_args), nullptr,
                                 true, true);
    if (rc) {
        LOG(ERROR) << "mke2fs returned " << rc;
        return false;
    }
    return true;
}

bool GsiService::AddPartitionFiemap(MetadataBuilder* builder, Partition* partition,
                                    FiemapWriter* writer) {
    for (const auto& extent : writer->extents()) {
        // :TODO: block size check for length, not sector size
        if (extent.fe_length % LP_SECTOR_SIZE != 0) {
            LOG(ERROR) << "Extent is not sector-aligned: " << extent.fe_length;
            return false;
        }
        if (extent.fe_physical % LP_SECTOR_SIZE != 0) {
            LOG(ERROR) << "Extent physical sector is not sector-aligned: " << extent.fe_physical;
            return false;
        }
        uint64_t num_sectors = extent.fe_length / LP_SECTOR_SIZE;
        uint64_t physical_sector = extent.fe_physical / LP_SECTOR_SIZE;
        if (!builder->AddLinearExtent(partition, "userdata", num_sectors, physical_sector)) {
            LOG(ERROR) << "Could not add extent to lp metadata";
            return false;
        }
    }
    return true;
}

bool GsiService::CreateBootableFile() {
    android::base::unique_fd fd(
            open(kGsiBootableFile, O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_CREAT, 0755));
    if (fd < 0) {
        LOG(ERROR) << "open: " << strerror(errno) << ": " << kGsiBootableFile;
        return false;
    }
    return true;
}

bool GsiService::EnsureFolderExists(const std::string& path) {
    if (!mkdir(path.c_str(), 0755) || errno == EEXIST) {
        return true;
    }

    LOG(ERROR) << "mkdir: " << strerror(errno) << ": " << path;
    return false;
}

}  // namespace gsi
}  // namespace android
