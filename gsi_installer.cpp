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

namespace android {
namespace gsi {

using namespace std::literals;
using namespace android::dm;
using namespace android::fiemap;
using namespace android::fs_mgr;
using android::base::unique_fd;

// The default size of userdata.img for GSI.
// We are looking for /data to have atleast 40% free space
static constexpr uint32_t kMinimumFreeSpaceThreshold = 40;
// Default userdata image size.
static constexpr int64_t kDefaultUserdataSize = int64_t(2) * 1024 * 1024 * 1024;

GsiInstaller::GsiInstaller(GsiService* service, const GsiInstallParams& params)
    : service_(service),
      install_dir_(params.installDir),
      gsi_size_(params.gsiSize),
      wipe_userdata_(params.wipeUserdata) {
    userdata_size_ = (params.userdataSize) ? params.userdataSize : kDefaultUserdataSize;
    images_ = ImageManager::Open(kDsuMetadataDir, install_dir_);

    // Only rm userdata_gsi if one didn't already exist.
    if (wipe_userdata_ || !images_->BackingImageExists("userdata_gsi")) {
        wipe_userdata_on_failure_ = true;
    }
}

GsiInstaller::GsiInstaller(GsiService* service, const std::string& install_dir)
    : service_(service), install_dir_(install_dir) {
    images_ = ImageManager::Open(kDsuMetadataDir, install_dir_);

    // The install already exists, so always mark it as succeeded.
    succeeded_ = true;
}

GsiInstaller::~GsiInstaller() {
    if (!succeeded_) {
        // Close open handles before we remove files.
        system_device_ = nullptr;
        PostInstallCleanup(images_.get());

        GsiService::RemoveGsiFiles(install_dir_, wipe_userdata_on_failure_);
    }
    if (IsAshmemMapped()) {
        UnmapAshmem();
    }
}

void GsiInstaller::PostInstallCleanup() {
    auto manager = ImageManager::Open(kDsuMetadataDir, GsiService::GetInstalledImageDir());
    if (!manager) {
        LOG(ERROR) << "Could not open image manager";
        return;
    }
    return PostInstallCleanup(manager.get());
}

void GsiInstaller::PostInstallCleanup(ImageManager* manager) {
    if (manager->IsImageMapped("userdata_gsi")) {
        manager->UnmapImageDevice("userdata_gsi");
    }
    if (manager->IsImageMapped("system_gsi")) {
        manager->UnmapImageDevice("system_gsi");
    }
}

int GsiInstaller::StartInstall() {
    if (int status = PerformSanityChecks()) {
        return status;
    }
    if (int status = PreallocateFiles()) {
        return status;
    }
    if (!FormatUserdata()) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    // Map system_gsi so we can write to it.
    system_device_ = OpenPartition("system_gsi");
    if (!system_device_) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    // Clear the progress indicator.
    service_->UpdateProgress(IGsiService::STATUS_NO_OPERATION, 0);
    return IGsiService::INSTALL_OK;
}

int GsiInstaller::PerformSanityChecks() {
    if (!images_) {
        LOG(ERROR) << "unable to create image manager";
        return IGsiService::INSTALL_ERROR_GENERIC;
    }
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
        images_->DeleteBackingImage("userdata_gsi");
    }
    images_->DeleteBackingImage("system_gsi");

    // Create fallocated files.
    if (int status = PreallocateUserdata()) {
        return status;
    }
    if (int status = PreallocateSystem()) {
        return status;
    }

    service_->UpdateProgress(IGsiService::STATUS_COMPLETE, 0);
    return IGsiService::INSTALL_OK;
}

int GsiInstaller::PreallocateUserdata() {
    if (wipe_userdata_ || !images_->BackingImageExists("userdata_gsi")) {
        service_->StartAsyncOperation("create userdata", userdata_size_);
        if (!CreateImage("userdata_gsi", userdata_size_, false)) {
            LOG(ERROR) << "Could not create userdata image";
            return IGsiService::INSTALL_ERROR_GENERIC;
        }

        // Signal that we need to reformat userdata.
        wipe_userdata_ = true;
    }
    return IGsiService::INSTALL_OK;
}

int GsiInstaller::PreallocateSystem() {
    service_->StartAsyncOperation("create system", gsi_size_);

    if (!CreateImage("system_gsi", gsi_size_, true)) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }
    return IGsiService::INSTALL_OK;
}

bool GsiInstaller::CreateImage(const std::string& name, uint64_t size, bool readonly) {
    auto progress = [this](uint64_t bytes, uint64_t /* total */) -> bool {
        service_->UpdateProgress(IGsiService::STATUS_WORKING, bytes);
        if (service_->should_abort()) return false;
        return true;
    };
    int flags = ImageManager::CREATE_IMAGE_DEFAULT;
    if (readonly) {
        flags |= ImageManager::CREATE_IMAGE_READONLY;
    }
    return images_->CreateBackingImage(name, size, flags, std::move(progress));
}

std::unique_ptr<MappedDevice> GsiInstaller::OpenPartition(const std::string& name) {
    return MappedDevice::Open(images_.get(), 10s, name);
}

bool GsiInstaller::CommitGsiChunk(int stream_fd, int64_t bytes) {
    service_->StartAsyncOperation("write gsi", gsi_size_);

    if (bytes < 0) {
        LOG(ERROR) << "chunk size " << bytes << " is negative";
        return false;
    }

    static const size_t kBlockSize = 4096;
    auto buffer = std::make_unique<char[]>(kBlockSize);

    int progress = -1;
    uint64_t remaining = bytes;
    while (remaining) {
        size_t max_to_read = std::min(static_cast<uint64_t>(kBlockSize), remaining);
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

bool GsiInstaller::IsFinishedWriting() {
    return gsi_bytes_written_ == gsi_size_;
}

bool GsiInstaller::IsAshmemMapped() {
    return ashmem_data_ != MAP_FAILED;
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
    if (!android::base::WriteFully(system_device_->fd(), data, bytes)) {
        PLOG(ERROR) << "write failed";
        return false;
    }
    gsi_bytes_written_ += bytes;
    return true;
}

bool GsiInstaller::MapAshmem(int fd, size_t size) {
    ashmem_size_ = size;
    ashmem_data_ = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    return ashmem_data_ != MAP_FAILED;
}

void GsiInstaller::UnmapAshmem() {
    if (munmap(ashmem_data_, ashmem_size_) != 0) {
        PLOG(ERROR) << "cannot munmap";
        return;
    }
    ashmem_data_ = MAP_FAILED;
    ashmem_size_ = -1;
}

bool GsiInstaller::CommitGsiChunk(size_t bytes) {
    if (!IsAshmemMapped()) {
        PLOG(ERROR) << "ashmem is not mapped";
        return false;
    }
    bool success = CommitGsiChunk(ashmem_data_, bytes);
    if (success && IsFinishedWriting()) {
        UnmapAshmem();
    }
    return success;
}

bool GsiInstaller::SetBootMode(bool one_shot) {
    if (one_shot) {
        if (!android::base::WriteStringToFile("1", kDsuOneShotBootFile)) {
            PLOG(ERROR) << "write " << kDsuOneShotBootFile;
            return false;
        }
    } else if (!access(kDsuOneShotBootFile, F_OK)) {
        std::string error;
        if (!android::base::RemoveFileIfExists(kDsuOneShotBootFile, &error)) {
            LOG(ERROR) << error;
            return false;
        }
    }
    return true;
}

bool GsiInstaller::CreateInstallStatusFile() {
    if (!android::base::WriteStringToFile("0", kDsuInstallStatusFile)) {
        PLOG(ERROR) << "write " << kDsuInstallStatusFile;
        return false;
    }
    return true;
}

bool GsiInstaller::FormatUserdata() {
    auto device = OpenPartition("userdata_gsi");
    if (!device) {
        return false;
    }

    // libcutils checks the first 4K, no matter the block size.
    std::string zeroes(4096, 0);
    if (!android::base::WriteFully(device->fd(), zeroes.data(), zeroes.size())) {
        PLOG(ERROR) << "write userdata_gsi";
        return false;
    }
    return true;
}

int GsiInstaller::SetGsiBootable(bool one_shot) {
    if (gsi_bytes_written_ != gsi_size_) {
        // We cannot boot if the image is incomplete.
        LOG(ERROR) << "image incomplete; expected " << gsi_size_ << " bytes, waiting for "
                   << (gsi_size_ - gsi_bytes_written_) << " bytes";
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    if (fsync(system_device_->fd())) {
        PLOG(ERROR) << "fsync failed for system_gsi";
        return IGsiService::INSTALL_ERROR_GENERIC;
    }
    system_device_ = {};

    // If files moved (are no longer pinned), the metadata file will be invalid.
    // This check can be removed once b/133967059 is fixed.
    if (!images_->Validate()) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    // Remember the installation directory.
    if (!android::base::WriteStringToFile(install_dir_, kDsuInstallDirFile)) {
        PLOG(ERROR) << "write failed: " << kDsuInstallDirFile;
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    // Note: create the install status file last, since this is the actual boot
    // indicator.
    if (!SetBootMode(one_shot) || !CreateInstallStatusFile()) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    succeeded_ = true;
    return IGsiService::INSTALL_OK;
}

int GsiInstaller::CheckInstallState() {
    std::vector<std::string> gsi_images = {"system_gsi", "userdata_gsi"};
    for (const auto& image : gsi_images) {
        if (!images_->PartitionExists(image) || !images_->BackingImageExists(image)) {
            return IGsiService::INSTALL_ERROR_GENERIC;
        }
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

    if (int error = CheckInstallState()) {
        return error;
    }
    if (!SetBootMode(one_shot) || !CreateInstallStatusFile()) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }
    return IGsiService::INSTALL_OK;
}

int GsiInstaller::WipeUserdata() {
    if (int error = CheckInstallState()) {
        return error;
    }

    auto device = OpenPartition("userdata_gsi");
    if (!device) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }

    // Wipe the first 1MiB of the device, ensuring both the first block and
    // the superblock are destroyed.
    static constexpr uint64_t kEraseSize = 1024 * 1024;

    std::string zeroes(4096, 0);
    uint64_t erase_size = std::min(kEraseSize, get_block_device_size(device->fd()));
    for (uint64_t i = 0; i < erase_size; i += zeroes.size()) {
        if (!android::base::WriteFully(device->fd(), zeroes.data(), zeroes.size())) {
            PLOG(ERROR) << "write userdata_gsi";
            return IGsiService::INSTALL_ERROR_GENERIC;
        }
    }
    return IGsiService::INSTALL_OK;
}

}  // namespace gsi
}  // namespace android
