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

#include "gsi_installer.h"

#include <sys/statvfs.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <ext4_utils/ext4_utils.h>
#include <fs_mgr_dm_linear.h>
#include <libdm/dm.h>
#include <libgsi/libgsi.h>

#include "file_paths.h"
#include "gsi_service.h"
#include "libgsi_private.h"
#include "utility.h"

namespace android {
namespace gsi {

using namespace std::literals;
using namespace android::dm;
using namespace android::fiemap_writer;
using namespace android::fs_mgr;
using android::base::unique_fd;

// The default size of userdata.img for GSI.
// We are looking for /data to have atleast 40% free space
static constexpr uint32_t kMinimumFreeSpaceThreshold = 40;
// We determine the fragmentation by making sure the files
// we create don't have more than 16 extents.
static constexpr uint32_t kMaximumExtents = 512;
// Default userdata image size.
static constexpr int64_t kDefaultUserdataSize = int64_t(2) * 1024 * 1024 * 1024;
static constexpr std::chrono::milliseconds kDmTimeout = 5000ms;

GsiInstaller::GsiInstaller(GsiService* service, const GsiInstallParams& params)
    : service_(service),
      install_dir_(params.installDir),
      gsi_size_(params.gsiSize),
      wipe_userdata_(params.wipeUserdata) {
    userdata_size_ = (params.userdataSize) ? params.userdataSize : kDefaultUserdataSize;
    userdata_gsi_path_ = GetImagePath("userdata_gsi");
    system_gsi_path_ = GetImagePath("system_gsi");

    // Only rm userdata_gsi if one didn't already exist.
    wipe_userdata_on_failure_ = wipe_userdata_ || access(userdata_gsi_path_.c_str(), F_OK);
}

GsiInstaller::GsiInstaller(GsiService* service, const std::string& install_dir)
    : service_(service), install_dir_(install_dir) {
    system_gsi_path_ = GetImagePath("system_gsi");

    // The install already exists, so always mark it as succeeded.
    succeeded_ = true;
}

GsiInstaller::~GsiInstaller() {
    if (!succeeded_) {
        // Close open handles before we remove files.
        system_writer_ = nullptr;
        partitions_.clear();
        PostInstallCleanup();

        GsiService::RemoveGsiFiles(install_dir_, wipe_userdata_on_failure_);
    }
}

void GsiInstaller::PostInstallCleanup() {
    const auto& dm = DeviceMapper::Instance();
    if (dm.GetState("userdata_gsi") != DmDeviceState::INVALID) {
        DestroyLogicalPartition("userdata_gsi");
    }
    if (dm.GetState("system_gsi") != DmDeviceState::INVALID) {
        DestroyLogicalPartition("system_gsi");
    }
}

int GsiInstaller::StartInstall() {
    if (int status = PerformSanityChecks()) {
        return status;
    }
    if (int status = PreallocateFiles()) {
        return status;
    }
    if (int status = DetermineReadWriteMethod()) {
        return status;
    }
    if (!FormatUserdata()) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    // Map system_gsi so we can write to it.
    system_writer_ = OpenPartition("system_gsi");
    if (!system_writer_) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    // Clear the progress indicator.
    service_->UpdateProgress(IGsiService::STATUS_NO_OPERATION, 0);
    return IGsiService::INSTALL_OK;
}

int GsiInstaller::DetermineReadWriteMethod() {
    // If there is a device-mapper node wrapping the block device, then we're
    // able to create another node around it; the dm layer does not carry the
    // exclusion lock down the stack when a mount occurs.
    //
    // If there is no intermediate device-mapper node, then partitions cannot be
    // opened writable due to sepolicy and exclusivity of having a mounted
    // filesystem. This should only happen on devices with no encryption, or
    // devices with FBE and no metadata encryption. For these cases it suffices
    // to perform normal file writes to /data/gsi (which is unencrypted).
    std::string block_device;
    if (!FiemapWriter::GetBlockDeviceForFile(system_gsi_path_.c_str(), &block_device,
                                             &can_use_devicemapper_)) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }
    if (install_dir_ != kDefaultGsiImageFolder && can_use_devicemapper_) {
        // Never use device-mapper on external media. We don't support adopted
        // storage yet, and accidentally using device-mapper could be dangerous
        // as we hardcode the userdata device as backing storage.
        LOG(ERROR) << "unexpected device-mapper node used to mount external media";
        return IGsiService::INSTALL_ERROR_GENERIC;
    }
    return IGsiService::INSTALL_OK;
}

int GsiInstaller::PerformSanityChecks() {
    if (gsi_size_ < 0) {
        LOG(ERROR) << "image size " << gsi_size_ << " is negative";
        return IGsiService::INSTALL_ERROR_GENERIC;
    }
    if (android::gsi::IsGsiRunning()) {
        LOG(ERROR) << "cannot install gsi inside a live gsi";
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    struct statvfs sb;
    if (statvfs(install_dir_.c_str(), &sb)) {
        PLOG(ERROR) << "failed to read file system stats";
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    // This is the same as android::vold::GetFreebytes() but we also
    // need the total file system size so we open code it here.
    uint64_t free_space = 1ULL * sb.f_bavail * sb.f_frsize;
    uint64_t fs_size = sb.f_blocks * sb.f_frsize;
    if (free_space <= (gsi_size_ + userdata_size_)) {
        LOG(ERROR) << "not enough free space (only " << free_space << " bytes available)";
        return IGsiService::INSTALL_ERROR_NO_SPACE;
    }
    // We are asking for 40% of the /data to be empty.
    // TODO: may be not hard code it like this
    double free_space_percent = ((1.0 * free_space) / fs_size) * 100;
    if (free_space_percent < kMinimumFreeSpaceThreshold) {
        LOG(ERROR) << "free space " << static_cast<uint64_t>(free_space_percent)
                   << "% is below the minimum threshold of " << kMinimumFreeSpaceThreshold << "%";
        return IGsiService::INSTALL_ERROR_FILE_SYSTEM_CLUTTERED;
    }
    return IGsiService::INSTALL_OK;
}

int GsiInstaller::PreallocateFiles() {
    if (wipe_userdata_) {
        SplitFiemap::RemoveSplitFiles(userdata_gsi_path_);
    }
    SplitFiemap::RemoveSplitFiles(system_gsi_path_);

    // TODO: trigger GC from fiemap writer.

    // Create fallocated files.
    if (int status = PreallocateUserdata()) {
        return status;
    }
    if (int status = PreallocateSystem()) {
        return status;
    }

    // Save the extent information in liblp.
    metadata_ = CreateMetadata();
    if (!metadata_) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    service_->UpdateProgress(IGsiService::STATUS_COMPLETE, 0);
    return IGsiService::INSTALL_OK;
}

int GsiInstaller::PreallocateUserdata() {
    int error;
    std::unique_ptr<SplitFiemap> userdata_image;
    if (wipe_userdata_ || access(userdata_gsi_path_.c_str(), F_OK)) {
        service_->StartAsyncOperation("create userdata", userdata_size_);
        userdata_image = CreateFiemapWriter(userdata_gsi_path_, userdata_size_, &error);
        if (!userdata_image) {
            LOG(ERROR) << "Could not create userdata image: " << userdata_gsi_path_;
            return error;
        }
        // Signal that we need to reformat userdata.
        wipe_userdata_ = true;
    } else {
        userdata_image = CreateFiemapWriter(userdata_gsi_path_, 0, &error);
        if (!userdata_image) {
            LOG(ERROR) << "Could not open userdata image: " << userdata_gsi_path_;
            return error;
        }
        if (userdata_size_ && userdata_image->size() < userdata_size_) {
            // :TODO: need to fallocate more blocks and resizefs.
        }
        userdata_size_ = userdata_image->size();
    }

    userdata_block_size_ = userdata_image->block_size();

    Image image = {
            .writer = std::move(userdata_image),
            .actual_size = userdata_size_,
    };
    partitions_.emplace(std::make_pair("userdata_gsi", std::move(image)));
    return IGsiService::INSTALL_OK;
}

int GsiInstaller::PreallocateSystem() {
    service_->StartAsyncOperation("create system", gsi_size_);

    int error;
    auto system_image = CreateFiemapWriter(system_gsi_path_, gsi_size_, &error);
    if (!system_image) {
        return error;
    }

    system_block_size_ = system_image->block_size();

    Image image = {
            .writer = std::move(system_image),
            .actual_size = gsi_size_,
    };
    partitions_.emplace(std::make_pair("system_gsi", std::move(image)));
    return IGsiService::INSTALL_OK;
}

std::unique_ptr<SplitFiemap> GsiInstaller::CreateFiemapWriter(const std::string& path,
                                                              uint64_t size, int* error) {
    bool create = (size != 0);

    std::function<bool(uint64_t, uint64_t)> progress;
    if (create) {
        progress = [this](uint64_t bytes, uint64_t /* total */) -> bool {
            service_->UpdateProgress(IGsiService::STATUS_WORKING, bytes);
            if (service_->should_abort()) return false;
            return true;
        };
    }

    std::unique_ptr<SplitFiemap> file;
    if (!size) {
        file = SplitFiemap::Open(path);
    } else {
        file = SplitFiemap::Create(path, size, 0, std::move(progress));
    }
    if (!file) {
        LOG(ERROR) << "failed to create or open " << path;
        *error = IGsiService::INSTALL_ERROR_GENERIC;
        return nullptr;
    }

    uint64_t extents = file->extents().size();
    if (extents > kMaximumExtents) {
        LOG(ERROR) << "file " << path << " has too many extents: " << extents;
        *error = IGsiService::INSTALL_ERROR_FILE_SYSTEM_CLUTTERED;
        return nullptr;
    }
    return file;
}

// Write data through an fd.
class FdWriter final : public GsiInstaller::WriteHelper {
  public:
    FdWriter(const std::string& path, unique_fd&& fd) : path_(path), fd_(std::move(fd)) {}

    bool Write(const void* data, uint64_t bytes) override {
        return android::base::WriteFully(fd_, data, bytes);
    }
    bool Flush() override {
        if (fsync(fd_)) {
            PLOG(ERROR) << "fsync failed: " << path_;
            return false;
        }
        return true;
    }
    uint64_t Size() override { return get_block_device_size(fd_); }

  private:
    std::string path_;
    unique_fd fd_;
};

// Write data through a SplitFiemap.
class SplitFiemapWriter final : public GsiInstaller::WriteHelper {
  public:
    explicit SplitFiemapWriter(SplitFiemap* writer) : writer_(writer) {}

    bool Write(const void* data, uint64_t bytes) override { return writer_->Write(data, bytes); }
    bool Flush() override { return writer_->Flush(); }
    uint64_t Size() override { return writer_->size(); }

  private:
    SplitFiemap* writer_;
};

std::unique_ptr<GsiInstaller::WriteHelper> GsiInstaller::OpenPartition(const std::string& name) {
    if (can_use_devicemapper_) {
        std::string path;
        if (!CreateLogicalPartition(kUserdataDevice, *metadata_.get(), name, true, kDmTimeout,
                                    &path)) {
            LOG(ERROR) << "Error creating device-mapper node for " << name;
            return {};
        }

        static const int kOpenFlags = O_RDWR | O_NOFOLLOW | O_CLOEXEC;
        unique_fd fd(open(path.c_str(), kOpenFlags));
        if (fd < 0) {
            PLOG(ERROR) << "could not open " << path;
        }
        return std::make_unique<FdWriter>(GetImagePath(name), std::move(fd));
    }

    auto iter = partitions_.find(name);
    if (iter == partitions_.end()) {
        LOG(ERROR) << "could not find partition " << name;
        return {};
    }
    return std::make_unique<SplitFiemapWriter>(iter->second.writer.get());
}

bool GsiInstaller::CommitGsiChunk(int stream_fd, int64_t bytes) {
    service_->StartAsyncOperation("write gsi", gsi_size_);

    if (bytes < 0) {
        LOG(ERROR) << "chunk size " << bytes << " is negative";
        return false;
    }

    auto buffer = std::make_unique<char[]>(system_block_size_);

    int progress = -1;
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

        // Only update the progress when the % (or permille, in this case)
        // significantly changes.
        int new_progress = ((gsi_size_ - remaining) * 1000) / gsi_size_;
        if (new_progress != progress) {
            service_->UpdateProgress(IGsiService::STATUS_WORKING, gsi_size_ - remaining);
        }
    }

    service_->UpdateProgress(IGsiService::STATUS_COMPLETE, gsi_size_);
    return true;
}

bool GsiInstaller::CommitGsiChunk(const void* data, size_t bytes) {
    if (static_cast<uint64_t>(bytes) > gsi_size_ - gsi_bytes_written_) {
        // We cannot write past the end of the image file.
        LOG(ERROR) << "chunk size " << bytes << " exceeds remaining image size (" << gsi_size_
                   << " expected, " << gsi_bytes_written_ << " written)";
        return false;
    }
    if (service_->should_abort()) {
        return false;
    }
    if (!system_writer_->Write(data, bytes)) {
        PLOG(ERROR) << "write failed";
        return false;
    }
    gsi_bytes_written_ += bytes;
    return true;
}

bool GsiInstaller::SetBootMode(bool one_shot) {
    if (one_shot) {
        if (!android::base::WriteStringToFile("1", kGsiOneShotBootFile)) {
            PLOG(ERROR) << "write " << kGsiOneShotBootFile;
            return false;
        }
    } else if (!access(kGsiOneShotBootFile, F_OK)) {
        std::string error;
        if (!android::base::RemoveFileIfExists(kGsiOneShotBootFile, &error)) {
            LOG(ERROR) << error;
            return false;
        }
    }
    return true;
}

std::string GsiInstaller::GetImagePath(const std::string& name) {
    return GsiService::GetImagePath(install_dir_, name);
}

bool GsiInstaller::CreateInstallStatusFile() {
    if (!android::base::WriteStringToFile("0", kGsiInstallStatusFile)) {
        PLOG(ERROR) << "write " << kGsiInstallStatusFile;
        return false;
    }
    return true;
}

std::unique_ptr<LpMetadata> GsiInstaller::CreateMetadata() {
    auto writer = partitions_["system_gsi"].writer.get();

    std::string data_device_path;
    if (install_dir_ == kDefaultGsiImageFolder && !access(kUserdataDevice, F_OK)) {
        auto actual_device = GetDevicePathForFile(writer);
        if (actual_device != kUserdataDevice) {
            LOG(ERROR) << "Image file did not resolve to userdata: " << actual_device;
            return nullptr;
        }
        data_device_path = actual_device;
    } else {
        data_device_path = writer->bdev_path();
    }
    auto data_device_name = android::base::Basename(data_device_path);

    PartitionOpener opener;
    BlockDeviceInfo data_device_info;
    if (!opener.GetInfo(data_device_path, &data_device_info)) {
        LOG(ERROR) << "Error reading userdata partition";
        return nullptr;
    }

    std::vector<BlockDeviceInfo> block_devices = {data_device_info};
    auto builder = MetadataBuilder::New(block_devices, data_device_name, 128 * 1024, 1);
    if (!builder) {
        LOG(ERROR) << "Error creating metadata builder";
        return nullptr;
    }

    for (const auto& [name, image] : partitions_) {
        uint32_t flags = LP_PARTITION_ATTR_NONE;
        if (name == "system_gsi") {
            flags |= LP_PARTITION_ATTR_READONLY;
        }
        Partition* partition = builder->AddPartition(name, flags);
        if (!partition) {
            LOG(ERROR) << "Error adding " << name << " to partition table";
            return nullptr;
        }
        if (!AddPartitionFiemap(builder.get(), partition, image, data_device_name)) {
            return nullptr;
        }
    }

    auto metadata = builder->Export();
    if (!metadata) {
        LOG(ERROR) << "Error exporting partition table";
        return nullptr;
    }
    return metadata;
}

bool GsiInstaller::CreateMetadataFile() {
    if (!WriteToImageFile(kGsiLpMetadataFile, *metadata_.get())) {
        LOG(ERROR) << "Error writing GSI partition table image";
        return false;
    }
    return true;
}

bool GsiInstaller::FormatUserdata() {
    auto writer = OpenPartition("userdata_gsi");
    if (!writer) {
        return false;
    }

    // libcutils checks the first 4K, no matter the block size.
    std::string zeroes(4096, 0);
    if (!writer->Write(zeroes.data(), zeroes.size())) {
        PLOG(ERROR) << "write userdata_gsi";
        return false;
    }
    return true;
}

bool GsiInstaller::AddPartitionFiemap(MetadataBuilder* builder, Partition* partition,
                                      const Image& image, const std::string& block_device) {
    uint64_t sectors_needed = image.actual_size / LP_SECTOR_SIZE;
    for (const auto& extent : image.writer->extents()) {
        // :TODO: block size check for length, not sector size
        if (extent.fe_length % LP_SECTOR_SIZE != 0) {
            LOG(ERROR) << "Extent is not sector-aligned: " << extent.fe_length;
            return false;
        }
        if (extent.fe_physical % LP_SECTOR_SIZE != 0) {
            LOG(ERROR) << "Extent physical sector is not sector-aligned: " << extent.fe_physical;
            return false;
        }

        uint64_t num_sectors =
                std::min(static_cast<uint64_t>(extent.fe_length / LP_SECTOR_SIZE), sectors_needed);
        if (!num_sectors || !sectors_needed) {
            // This should never happen, but we include it just in case. It would
            // indicate that the last filesystem block had multiple extents.
            LOG(WARNING) << "FiemapWriter allocated extra blocks";
            break;
        }

        uint64_t physical_sector = extent.fe_physical / LP_SECTOR_SIZE;
        if (!builder->AddLinearExtent(partition, block_device, num_sectors, physical_sector)) {
            LOG(ERROR) << "Could not add extent to lp metadata";
            return false;
        }

        sectors_needed -= num_sectors;
    }
    return true;
}

static uint64_t GetPartitionSize(const LpMetadata& metadata, const std::string& name) {
    const LpMetadataPartition* partition = FindPartition(metadata, name);
    if (!partition) {
        return 0;
    }
    return android::fs_mgr::GetPartitionSize(metadata, *partition);
}

int GsiInstaller::GetExistingImage(const LpMetadata& metadata, const std::string& name,
                                   Image* image) {
    int error;
    std::string path = GetImagePath(name);
    auto writer = CreateFiemapWriter(path.c_str(), 0, &error);
    if (!writer) {
        return error;
    }

    // Even after recovering the FIEMAP, we also need to know the exact intended
    // size of the image, since FiemapWriter may have extended the final block.
    uint64_t actual_size = GetPartitionSize(metadata, name);
    if (!actual_size) {
        LOG(ERROR) << "Could not determine the pre-existing size of " << name;
        return IGsiService::INSTALL_ERROR_GENERIC;
    }
    image->writer = std::move(writer);
    image->actual_size = actual_size;
    return IGsiService::INSTALL_OK;
}

int GsiInstaller::SetGsiBootable(bool one_shot) {
    if (gsi_bytes_written_ != gsi_size_) {
        // We cannot boot if the image is incomplete.
        LOG(ERROR) << "image incomplete; expected " << gsi_size_ << " bytes, waiting for "
                   << (gsi_size_ - gsi_bytes_written_) << " bytes";
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    if (!system_writer_->Flush()) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    // If files moved (are no longer pinned), the metadata file will be invalid.
    for (const auto& [name, image] : partitions_) {
        if (!image.writer->HasPinnedExtents()) {
            LOG(ERROR) << name << " no longer has pinned extents";
            return IGsiService::INSTALL_ERROR_GENERIC;
        }
    }

    // Remember the installation directory.
    if (!android::base::WriteStringToFile(install_dir_, kGsiInstallDirFile)) {
        PLOG(ERROR) << "write failed: " << kGsiInstallDirFile;
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    // Note: create the install status file last, since this is the actual boot
    // indicator.
    if (!CreateMetadataFile() || !SetBootMode(one_shot) || !CreateInstallStatusFile()) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    succeeded_ = true;
    return IGsiService::INSTALL_OK;
}

int GsiInstaller::RebuildInstallState() {
    if (int error = DetermineReadWriteMethod()) {
        return error;
    }

    // Note: this metadata is only used to recover the original partition sizes.
    // We do not trust the extent information, which will get rebuilt later.
    auto old_metadata = ReadFromImageFile(kGsiLpMetadataFile);
    if (!old_metadata) {
        LOG(ERROR) << "GSI install is incomplete";
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    // Recover parition information.
    Image userdata_image;
    if (int error = GetExistingImage(*old_metadata.get(), "userdata_gsi", &userdata_image)) {
        return error;
    }
    partitions_.emplace(std::make_pair("userdata_gsi", std::move(userdata_image)));

    Image system_image;
    if (int error = GetExistingImage(*old_metadata.get(), "system_gsi", &system_image)) {
        return error;
    }
    partitions_.emplace(std::make_pair("system_gsi", std::move(system_image)));

    metadata_ = CreateMetadata();
    if (!metadata_) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }
    return IGsiService::INSTALL_OK;
}

int GsiInstaller::ReenableGsi(bool one_shot) {
    if (IsGsiRunning()) {
        if (!SetBootMode(one_shot) || !CreateInstallStatusFile()) {
            return IGsiService::INSTALL_ERROR_GENERIC;
        }
        return IGsiService::INSTALL_OK;
    }

    if (int error = RebuildInstallState()) {
        return error;
    }
    if (!CreateMetadataFile() || !SetBootMode(one_shot) || !CreateInstallStatusFile()) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }
    return IGsiService::INSTALL_OK;
}

int GsiInstaller::WipeUserdata() {
    if (int error = RebuildInstallState()) {
        return error;
    }

    auto writer = OpenPartition("userdata_gsi");
    if (!writer) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    // Wipe the first 1MiB of the device, ensuring both the first block and
    // the superblock are destroyed.
    static constexpr uint64_t kEraseSize = 1024 * 1024;

    std::string zeroes(4096, 0);
    uint64_t erase_size = std::min(kEraseSize, writer->Size());
    for (uint64_t i = 0; i < erase_size; i += zeroes.size()) {
        if (!writer->Write(zeroes.data(), zeroes.size())) {
            PLOG(ERROR) << "write userdata_gsi";
            return IGsiService::INSTALL_ERROR_GENERIC;
        }
    }
    return IGsiService::INSTALL_OK;
}

}  // namespace gsi
}  // namespace android
