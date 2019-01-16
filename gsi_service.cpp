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
#include <linux/fs.h>
#include <sys/ioctl.h>
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
#include "libgsi_private.h"

namespace android {
namespace gsi {

using namespace std::literals;
using namespace android::fs_mgr;
using namespace android::fiemap_writer;

static constexpr char kUserdataDevice[] = "/dev/block/by-name/userdata";

// The default size of userdata.img for GSI.
// We are looking for /data to have atleast 40% free space
static constexpr uint32_t kMinimumFreeSpaceThreshold = 40;
// We determine the fragmentation by making sure the files
// we create don't have more than 16 extents.
static constexpr uint32_t kMaximumExtents = 512;
// Default userdata image size.
static constexpr int64_t kDefaultUserdataSize = int64_t(8) * 1024 * 1024 * 1024;
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

binder::Status GsiService::startGsiInstall(int64_t gsiSize, int64_t userdataSize, bool wipeUserdata,
                                           bool* _aidl_return) {
    std::lock_guard<std::mutex> guard(main_lock_);

    // Make sure any interrupted installations are cleaned up.
    PostInstallCleanup();

    // Only rm userdata_gsi if one didn't already exist.
    wipe_userdata_on_failure_ = wipeUserdata || access(kUserdataFile, F_OK);

    if (!StartInstall(gsiSize, userdataSize, wipeUserdata)) {
        // Perform local cleanup and delete any lingering files.
        PostInstallCleanup();
        RemoveGsiFiles(wipe_userdata_on_failure_);
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

    if (installing_) {
        *_aidl_return = SetGsiBootable();
    } else {
        *_aidl_return = ReenableGsi();
    }
    return binder::Status::ok();
}

binder::Status GsiService::removeGsiInstall(bool* _aidl_return) {
    std::lock_guard<std::mutex> guard(main_lock_);

    // Just in case an install was left hanging.
    PostInstallCleanup();

    if (IsGsiRunning()) {
        // Can't remove gsi files while running.
        *_aidl_return = UninstallGsi();
    } else {
        *_aidl_return = RemoveGsiFiles(true /* wipeUserdata */);
    }
    return binder::Status::ok();
}

binder::Status GsiService::disableGsiInstall(bool* _aidl_return) {
    std::lock_guard<std::mutex> guard(main_lock_);

    *_aidl_return = DisableGsiInstall();
    return binder::Status::ok();
}

binder::Status GsiService::isGsiRunning(bool* _aidl_return) {
    std::lock_guard<std::mutex> guard(main_lock_);

    *_aidl_return = IsGsiRunning();
    return binder::Status::ok();
}

binder::Status GsiService::isGsiInstalled(bool* _aidl_return) {
    std::lock_guard<std::mutex> guard(main_lock_);

    *_aidl_return = IsGsiInstalled();
    return binder::Status::ok();
}

binder::Status GsiService::isGsiInstallInProgress(bool* _aidl_return) {
    std::lock_guard<std::mutex> guard(main_lock_);

    *_aidl_return = installing_;
    return binder::Status::ok();
}

binder::Status GsiService::cancelGsiInstall(bool* _aidl_return) {
    std::lock_guard<std::mutex> guard(main_lock_);

    if (!installing_) {
        LOG(ERROR) << "No GSI installation in progress to cancel";
        *_aidl_return = false;
        return binder::Status::ok();
    }

    PostInstallCleanup();
    RemoveGsiFiles(wipe_userdata_on_failure_);

    *_aidl_return = true;
    return binder::Status::ok();
}

binder::Status GsiService::getUserdataImageSize(int64_t* _aidl_return) {
    *_aidl_return = -1;

    if (installing_) {
        // Size has already been computed.
        *_aidl_return = userdata_size_;
    } else if (IsGsiRunning()) {
        // :TODO: libdm
        android::base::unique_fd fd(open(kUserdataDevice, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (fd < 0) {
            PLOG(ERROR) << "open " << kUserdataDevice;
            return binder::Status::ok();
        }

        int64_t size;
        if (ioctl(fd, BLKGETSIZE64, &size)) {
            PLOG(ERROR) << "BLKGETSIZE64 " << kUserdataDevice;
            return binder::Status::ok();
        }
        *_aidl_return = size;
    } else {
        // Stat the size of the userdata file.
        struct stat s;
        if (stat(kUserdataFile, &s)) {
            if (errno != ENOENT) {
                PLOG(ERROR) << "open " << kUserdataFile;
                return binder::Status::ok();
            }
            *_aidl_return = 0;
        } else {
            *_aidl_return = s.st_size;
        }
    }
    return binder::Status::ok();
}

void GsiService::PostInstallCleanup() {
    // This must be closed before unmapping partitions.
    system_fd_ = {};

    DestroyLogicalPartition("userdata_gsi", kDmTimeout);
    DestroyLogicalPartition("system_gsi", kDmTimeout);

    installing_ = false;
}

bool GsiService::StartInstall(int64_t gsi_size, int64_t userdata_size, bool wipe_userdata) {
    gsi_size_ = gsi_size;
    userdata_size_ = userdata_size;
    wipe_userdata_ = wipe_userdata;

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
    if (wipe_userdata_) {
        android::base::RemoveFileIfExists(kUserdataFile);
    }
    android::base::RemoveFileIfExists(kSystemFile);

    // TODO: trigger GC from fiemap writer.

    // Create fallocated files.
    FiemapUniquePtr userdata_image;
    if (wipe_userdata_ || access(kUserdataFile, F_OK)) {
        if (!userdata_size_) {
            userdata_size_ = kDefaultUserdataSize;
        }
        userdata_image = CreateFiemapWriter(kUserdataFile, userdata_size_);
        if (!userdata_image) {
            LOG(ERROR) << "Could not create userdata image: " << kUserdataFile;
            return false;
        }
        // Signal that we need to reformat userdata.
        wipe_userdata_ = true;
    } else {
        userdata_image = CreateFiemapWriter(kUserdataFile, 0);
        if (!userdata_image) {
            LOG(ERROR) << "Could not open userdata image: " << kUserdataFile;
            return false;
        }
        if (userdata_size_ && userdata_image->size() < userdata_size_) {
            // :TODO: need to fallocate more blocks and resizefs.
        }
        userdata_size_ = userdata_image->size();
    }

    auto system_image = CreateFiemapWriter(kSystemFile, gsi_size_);
    if (!system_image) {
        return false;
    }

    // Save the extent information in liblp.
    metadata_ = CreateMetadata(userdata_image.get(), system_image.get());
    if (!metadata_) {
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
    bool create = (size != 0);
    auto file = FiemapWriter::Open(path, size, create);
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

    if (!CreateMetadataFile(*metadata_.get()) || !CreateInstallStatusFile()) {
        return false;
    }

    PostInstallCleanup();
    return true;
}

bool GsiService::ReenableGsi() {
    if (!android::gsi::IsGsiInstalled()) {
        LOG(ERROR) << "no gsi installed - cannot re-enable";
        return false;
    }

    std::string boot_key;
    if (!GetInstallStatus(&boot_key)) {
        PLOG(ERROR) << "read " << kGsiInstallStatusFile;
        return false;
    }
    if (boot_key != kInstallStatusDisabled) {
        LOG(ERROR) << "GSI is not currently disabled";
        return false;
    }

    auto userdata_image = CreateFiemapWriter(kUserdataFile, 0);
    if (!userdata_image) {
        LOG(ERROR) << "could not find userdata image";
        return false;
    }
    auto system_image = CreateFiemapWriter(kSystemFile, 0);
    if (!system_image) {
        LOG(ERROR) << "could not find system image";
        return false;
    }

    auto metadata = CreateMetadata(userdata_image.get(), system_image.get());
    if (!metadata) {
        return false;
    }
    if (!CreateMetadataFile(*metadata.get()) || !CreateInstallStatusFile()) {
        return false;
    }
    return true;
}

bool GsiService::RemoveGsiFiles(bool wipeUserdata) {
    std::vector<std::string> files{
            kSystemFile,
            kGsiInstallStatusFile,
            kGsiLpMetadataFile,
    };
    if (wipeUserdata) {
        files.emplace_back(kUserdataFile);
    }

    bool ok = true;
    for (const auto& file : files) {
        std::string message;
        if (!android::base::RemoveFileIfExists(file, &message)) {
            LOG(ERROR) << message;
            ok = false;
        }
    }
    return ok;
}

bool GsiService::DisableGsiInstall() {
    if (!android::gsi::IsGsiInstalled()) {
        LOG(ERROR) << "cannot disable gsi install - no install detected";
        return false;
    }
    if (installing_) {
        LOG(ERROR) << "cannot disable gsi during GSI installation";
        return false;
    }
    if (!DisableGsi()) {
        PLOG(ERROR) << "could not write gsi status";
        return false;
    }
    return true;
}

std::unique_ptr<LpMetadata> GsiService::CreateMetadata(FiemapWriter* userdata_image,
                                                       FiemapWriter* system_image) {
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

    Partition* userdata = builder->AddPartition("userdata_gsi", LP_PARTITION_ATTR_NONE);
    Partition* system = builder->AddPartition("system_gsi", LP_PARTITION_ATTR_READONLY);
    if (!userdata || !system) {
        LOG(ERROR) << "Error creating partition table";
        return nullptr;
    }
    if (!AddPartitionFiemap(builder.get(), userdata, userdata_image) ||
        !AddPartitionFiemap(builder.get(), system, system_image)) {
        return nullptr;
    }

    auto metadata = builder->Export();
    if (!metadata) {
        LOG(ERROR) << "Error exporting partition table";
        return nullptr;
    }
    return metadata;
}

bool GsiService::CreateMetadataFile(const LpMetadata& metadata) {
    if (!WriteToImageFile(kGsiLpMetadataFile, metadata)) {
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

bool GsiService::CreateInstallStatusFile() {
    if (!android::base::WriteStringToFile("0", kGsiInstallStatusFile)) {
        PLOG(ERROR) << "write " << kGsiInstallStatusFile;
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

void GsiService::RunStartupTasks() {
    if (!IsGsiInstalled()) {
        return;
    }

    std::string boot_key;
    if (!GetInstallStatus(&boot_key)) {
        PLOG(ERROR) << "read " << kGsiInstallStatusFile;
        return;
    }

    if (!IsGsiRunning()) {
        // Check if a wipe was requested from fastboot or adb-in-gsi.
        if (boot_key == kInstallStatusWipe) {
            RemoveGsiFiles(true /* wipeUserdata */);
        }
    } else {
        int ignore;
        if (GetBootAttempts(boot_key, &ignore)) {
            // Mark the GSI as having successfully booted.
            if (!android::base::WriteStringToFile(kInstallStatusOk, kGsiInstallStatusFile)) {
                PLOG(ERROR) << "write " << kGsiInstallStatusFile;
            }
        }
    }
}

}  // namespace gsi
}  // namespace android
