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
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android/gsi/BnImageService.h>
#include <android/gsi/IGsiService.h>
#include <ext4_utils/ext4_utils.h>
#include <fs_mgr.h>
#include <libdm/dm.h>
#include <libfiemap/image_manager.h>
#include <private/android_filesystem_config.h>

#include "file_paths.h"
#include "libgsi_private.h"

namespace android {
namespace gsi {

using namespace std::literals;
using namespace android::fs_mgr;
using namespace android::fiemap;
using android::base::ReadFileToString;
using android::base::RemoveFileIfExists;
using android::base::Split;
using android::base::StringPrintf;
using android::base::unique_fd;
using android::base::WriteStringToFd;
using android::base::WriteStringToFile;
using android::dm::DeviceMapper;

android::wp<GsiService> GsiService::sInstance;

// Default userdata image size.
static constexpr int64_t kDefaultUserdataSize = int64_t(2) * 1024 * 1024 * 1024;

void Gsid::Register() {
    auto ret = android::BinderService<Gsid>::publish();
    if (ret != android::OK) {
        LOG(FATAL) << "Could not register gsi service: " << ret;
    }
}

binder::Status Gsid::getClient(android::sp<IGsiService>* _aidl_return) {
    *_aidl_return = GsiService::Get(this);
    return binder::Status::ok();
}

GsiService::GsiService(Gsid* parent) : parent_(parent) {
    progress_ = {};
}

GsiService::~GsiService() {
    std::lock_guard<std::mutex> guard(parent_->lock());

    if (sInstance == this) {
        // No more consumers, gracefully shut down gsid.
        exit(0);
    }
}

android::sp<IGsiService> GsiService::Get(Gsid* parent) {
    std::lock_guard<std::mutex> guard(parent->lock());

    android::sp<GsiService> service = sInstance.promote();
    if (!service) {
        service = new GsiService(parent);
        sInstance = service.get();
    }
    return service.get();
}

#define ENFORCE_SYSTEM                      \
    do {                                    \
        binder::Status status = CheckUid(); \
        if (!status.isOk()) return status;  \
    } while (0)

#define ENFORCE_SYSTEM_OR_SHELL                                       \
    do {                                                              \
        binder::Status status = CheckUid(AccessLevel::SystemOrShell); \
        if (!status.isOk()) return status;                            \
    } while (0)

int GsiService::SaveInstallation(const std::string& installation) {
    auto fd = android::base::unique_fd(
            open(kDsuInstallDirFile, O_RDWR | O_SYNC | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR));
    if (!WriteStringToFd(installation, fd)) {
        PLOG(ERROR) << "write failed: " << kDsuInstallDirFile;
        return INSTALL_ERROR_GENERIC;
    }
    return INSTALL_OK;
}

binder::Status GsiService::openInstall(const std::string& install_dir, int* _aidl_return) {
    ENFORCE_SYSTEM;
    std::lock_guard<std::mutex> guard(parent_->lock());
    if (IsGsiRunning()) {
        *_aidl_return = IGsiService::INSTALL_ERROR_GENERIC;
        return binder::Status::ok();
    }
    install_dir_ = install_dir;
    if (int status = ValidateInstallParams(install_dir_)) {
        *_aidl_return = status;
        return binder::Status::ok();
    }
    std::string message;
    if (!RemoveFileIfExists(GetCompleteIndication(install_dir_), &message)) {
        LOG(ERROR) << message;
    }
    // Remember the installation directory before allocate any resource
    *_aidl_return = SaveInstallation(install_dir_);
    return binder::Status::ok();
}

binder::Status GsiService::closeInstall(int* _aidl_return) {
    ENFORCE_SYSTEM;
    std::lock_guard<std::mutex> guard(parent_->lock());
    std::string file = GetCompleteIndication(install_dir_);
    if (!WriteStringToFile("OK", file)) {
        PLOG(ERROR) << "write failed: " << file;
        *_aidl_return = INSTALL_ERROR_GENERIC;
    }
    install_dir_ = {};
    *_aidl_return = INSTALL_OK;
    return binder::Status::ok();
}

binder::Status GsiService::createPartition(const ::std::string& name, int64_t size, bool readOnly,
                                           int32_t* _aidl_return) {
    ENFORCE_SYSTEM;
    std::lock_guard<std::mutex> guard(parent_->lock());

    if (install_dir_.empty()) {
        PLOG(ERROR) << "open is required for createPartition";
        *_aidl_return = INSTALL_ERROR_GENERIC;
        return binder::Status::ok();
    }

    // Make sure a pending interrupted installations are cleaned up.
    installer_ = nullptr;

    // Do some precursor validation on the arguments before diving into the
    // install process.
    if (size % LP_SECTOR_SIZE) {
        LOG(ERROR) << " size " << size << " is not a multiple of " << LP_SECTOR_SIZE;
        *_aidl_return = INSTALL_ERROR_GENERIC;
        return binder::Status::ok();
    }

    if (size == 0 && name == "userdata") {
        size = kDefaultUserdataSize;
    }
    installer_ = std::make_unique<PartitionInstaller>(this, install_dir_, name, size, readOnly);
    int status = installer_->StartInstall();
    if (status != INSTALL_OK) {
        installer_ = nullptr;
    }
    *_aidl_return = status;
    return binder::Status::ok();
}

binder::Status GsiService::commitGsiChunkFromStream(const android::os::ParcelFileDescriptor& stream,
                                                    int64_t bytes, bool* _aidl_return) {
    ENFORCE_SYSTEM;
    std::lock_guard<std::mutex> guard(parent_->lock());

    if (!installer_) {
        *_aidl_return = false;
        return binder::Status::ok();
    }

    *_aidl_return = installer_->CommitGsiChunk(stream.get(), bytes);
    return binder::Status::ok();
}

void GsiService::StartAsyncOperation(const std::string& step, int64_t total_bytes) {
    std::lock_guard<std::mutex> guard(progress_lock_);

    progress_.step = step;
    progress_.status = STATUS_WORKING;
    progress_.bytes_processed = 0;
    progress_.total_bytes = total_bytes;
}

void GsiService::UpdateProgress(int status, int64_t bytes_processed) {
    std::lock_guard<std::mutex> guard(progress_lock_);

    progress_.status = status;
    if (status == STATUS_COMPLETE) {
        progress_.bytes_processed = progress_.total_bytes;
    } else {
        progress_.bytes_processed = bytes_processed;
    }
}

binder::Status GsiService::getInstallProgress(::android::gsi::GsiProgress* _aidl_return) {
    ENFORCE_SYSTEM;
    std::lock_guard<std::mutex> guard(progress_lock_);

    *_aidl_return = progress_;
    return binder::Status::ok();
}

binder::Status GsiService::commitGsiChunkFromAshmem(int64_t bytes, bool* _aidl_return) {
    ENFORCE_SYSTEM;
    std::lock_guard<std::mutex> guard(parent_->lock());

    if (!installer_) {
        *_aidl_return = false;
        return binder::Status::ok();
    }
    *_aidl_return = installer_->CommitGsiChunk(bytes);
    return binder::Status::ok();
}

binder::Status GsiService::setGsiAshmem(const ::android::os::ParcelFileDescriptor& ashmem,
                                        int64_t size, bool* _aidl_return) {
    if (!installer_) {
        *_aidl_return = false;
        return binder::Status::ok();
    }
    *_aidl_return = installer_->MapAshmem(ashmem.get(), size);
    return binder::Status::ok();
}

binder::Status GsiService::enableGsi(bool one_shot, int* _aidl_return) {
    std::lock_guard<std::mutex> guard(parent_->lock());

    if (installer_) {
        ENFORCE_SYSTEM;
        installer_ = {};
        // Note: create the install status file last, since this is the actual boot
        // indicator.
        if (!SetBootMode(one_shot) || !CreateInstallStatusFile()) {
            *_aidl_return = IGsiService::INSTALL_ERROR_GENERIC;
        } else {
            *_aidl_return = INSTALL_OK;
        }
    } else {
        ENFORCE_SYSTEM_OR_SHELL;
        *_aidl_return = ReenableGsi(one_shot);
    }

    installer_ = nullptr;
    return binder::Status::ok();
}

binder::Status GsiService::isGsiEnabled(bool* _aidl_return) {
    ENFORCE_SYSTEM_OR_SHELL;
    std::lock_guard<std::mutex> guard(parent_->lock());
    std::string boot_key;
    if (!GetInstallStatus(&boot_key)) {
        *_aidl_return = false;
    } else {
        *_aidl_return = (boot_key == kInstallStatusOk);
    }
    return binder::Status::ok();
}

binder::Status GsiService::removeGsi(bool* _aidl_return) {
    ENFORCE_SYSTEM_OR_SHELL;
    std::lock_guard<std::mutex> guard(parent_->lock());

    std::string install_dir = GetActiveInstalledImageDir();
    if (IsGsiRunning()) {
        // Can't remove gsi files while running.
        *_aidl_return = UninstallGsi();
    } else {
        *_aidl_return = RemoveGsiFiles(install_dir);
    }
    return binder::Status::ok();
}

binder::Status GsiService::disableGsi(bool* _aidl_return) {
    ENFORCE_SYSTEM_OR_SHELL;
    std::lock_guard<std::mutex> guard(parent_->lock());

    *_aidl_return = DisableGsiInstall();
    return binder::Status::ok();
}

binder::Status GsiService::isGsiRunning(bool* _aidl_return) {
    ENFORCE_SYSTEM_OR_SHELL;
    std::lock_guard<std::mutex> guard(parent_->lock());

    *_aidl_return = IsGsiRunning();
    return binder::Status::ok();
}

binder::Status GsiService::isGsiInstalled(bool* _aidl_return) {
    ENFORCE_SYSTEM_OR_SHELL;
    std::lock_guard<std::mutex> guard(parent_->lock());

    *_aidl_return = IsGsiInstalled();
    return binder::Status::ok();
}

binder::Status GsiService::isGsiInstallInProgress(bool* _aidl_return) {
    ENFORCE_SYSTEM_OR_SHELL;
    std::lock_guard<std::mutex> guard(parent_->lock());

    *_aidl_return = !!installer_;
    return binder::Status::ok();
}

binder::Status GsiService::cancelGsiInstall(bool* _aidl_return) {
    ENFORCE_SYSTEM;
    should_abort_ = true;
    std::lock_guard<std::mutex> guard(parent_->lock());

    should_abort_ = false;
    installer_ = nullptr;

    *_aidl_return = true;
    return binder::Status::ok();
}

binder::Status GsiService::getInstalledGsiImageDir(std::string* _aidl_return) {
    ENFORCE_SYSTEM;
    std::lock_guard<std::mutex> guard(parent_->lock());

    *_aidl_return = GetActiveInstalledImageDir();
    return binder::Status::ok();
}

binder::Status GsiService::zeroPartition(const std::string& name, int* _aidl_return) {
    ENFORCE_SYSTEM_OR_SHELL;
    std::lock_guard<std::mutex> guard(parent_->lock());

    if (IsGsiRunning() || !IsGsiInstalled()) {
        *_aidl_return = IGsiService::INSTALL_ERROR_GENERIC;
        return binder::Status::ok();
    }

    std::string install_dir = GetActiveInstalledImageDir();
    *_aidl_return = PartitionInstaller::WipeWritable(install_dir, name);

    return binder::Status::ok();
}

static binder::Status BinderError(const std::string& message) {
    return binder::Status::fromExceptionCode(binder::Status::EX_SERVICE_SPECIFIC,
                                             String8(message.c_str()));
}

binder::Status GsiService::dumpDeviceMapperDevices(std::string* _aidl_return) {
    ENFORCE_SYSTEM_OR_SHELL;

    auto& dm = DeviceMapper::Instance();

    std::vector<DeviceMapper::DmBlockDevice> devices;
    if (!dm.GetAvailableDevices(&devices)) {
        return BinderError("Could not list devices");
    }

    std::stringstream text;
    for (const auto& device : devices) {
        text << "Device " << device.name() << " (" << device.Major() << ":" << device.Minor()
             << ")\n";

        std::vector<DeviceMapper::TargetInfo> table;
        if (!dm.GetTableInfo(device.name(), &table)) {
            continue;
        }

        for (const auto& target : table) {
            const auto& spec = target.spec;
            auto target_type = DeviceMapper::GetTargetType(spec);
            text << "    " << target_type << " " << spec.sector_start << " " << spec.length << " "
                 << target.data << "\n";
        }
    }

    *_aidl_return = text.str();
    return binder::Status::ok();
}

bool GsiService::CreateInstallStatusFile() {
    if (!android::base::WriteStringToFile("0", kDsuInstallStatusFile)) {
        PLOG(ERROR) << "write " << kDsuInstallStatusFile;
        return false;
    }
    return true;
}

bool GsiService::SetBootMode(bool one_shot) {
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

static binder::Status UidSecurityError() {
    uid_t uid = IPCThreadState::self()->getCallingUid();
    auto message = StringPrintf("UID %d is not allowed", uid);
    return binder::Status::fromExceptionCode(binder::Status::EX_SECURITY, String8(message.c_str()));
}

class ImageService : public BinderService<ImageService>, public BnImageService {
  public:
    ImageService(GsiService* service, std::unique_ptr<ImageManager>&& impl, uid_t uid);
    binder::Status getAllBackingImages(std::vector<std::string>* _aidl_return);
    binder::Status createBackingImage(const std::string& name, int64_t size, int flags) override;
    binder::Status deleteBackingImage(const std::string& name) override;
    binder::Status mapImageDevice(const std::string& name, int32_t timeout_ms,
                                  MappedImage* mapping) override;
    binder::Status unmapImageDevice(const std::string& name) override;
    binder::Status backingImageExists(const std::string& name, bool* _aidl_return) override;
    binder::Status isImageMapped(const std::string& name, bool* _aidl_return) override;
    binder::Status zeroFillNewImage(const std::string& name, int64_t bytes) override;
    binder::Status removeAllImages() override;
    binder::Status removeDisabledImages() override;
    binder::Status getMappedImageDevice(const std::string& name, std::string* device) override;

  private:
    bool CheckUid();

    android::sp<GsiService> service_;
    android::sp<Gsid> parent_;
    std::unique_ptr<ImageManager> impl_;
    uid_t uid_;
};

ImageService::ImageService(GsiService* service, std::unique_ptr<ImageManager>&& impl, uid_t uid)
    : service_(service), parent_(service->parent()), impl_(std::move(impl)), uid_(uid) {}

binder::Status ImageService::getAllBackingImages(std::vector<std::string>* _aidl_return) {
    *_aidl_return = impl_->GetAllBackingImages();
    return binder::Status::ok();
}

binder::Status ImageService::createBackingImage(const std::string& name, int64_t size, int flags) {
    if (!CheckUid()) return UidSecurityError();

    std::lock_guard<std::mutex> guard(parent_->lock());

    if (!impl_->CreateBackingImage(name, size, flags, nullptr)) {
        return BinderError("Failed to create");
    }
    return binder::Status::ok();
}

binder::Status ImageService::deleteBackingImage(const std::string& name) {
    if (!CheckUid()) return UidSecurityError();

    std::lock_guard<std::mutex> guard(parent_->lock());

    if (!impl_->DeleteBackingImage(name)) {
        return BinderError("Failed to delete");
    }
    return binder::Status::ok();
}

binder::Status ImageService::mapImageDevice(const std::string& name, int32_t timeout_ms,
                                            MappedImage* mapping) {
    if (!CheckUid()) return UidSecurityError();

    std::lock_guard<std::mutex> guard(parent_->lock());

    if (!impl_->MapImageDevice(name, std::chrono::milliseconds(timeout_ms), &mapping->path)) {
        return BinderError("Failed to map");
    }
    return binder::Status::ok();
}

binder::Status ImageService::unmapImageDevice(const std::string& name) {
    if (!CheckUid()) return UidSecurityError();

    std::lock_guard<std::mutex> guard(parent_->lock());

    if (!impl_->UnmapImageDevice(name)) {
        return BinderError("Failed to unmap");
    }
    return binder::Status::ok();
}

binder::Status ImageService::backingImageExists(const std::string& name, bool* _aidl_return) {
    if (!CheckUid()) return UidSecurityError();

    std::lock_guard<std::mutex> guard(parent_->lock());

    *_aidl_return = impl_->BackingImageExists(name);
    return binder::Status::ok();
}

binder::Status ImageService::isImageMapped(const std::string& name, bool* _aidl_return) {
    if (!CheckUid()) return UidSecurityError();

    std::lock_guard<std::mutex> guard(parent_->lock());

    *_aidl_return = impl_->IsImageMapped(name);
    return binder::Status::ok();
}

binder::Status ImageService::zeroFillNewImage(const std::string& name, int64_t bytes) {
    if (!CheckUid()) return UidSecurityError();

    std::lock_guard<std::mutex> guard(parent_->lock());

    if (bytes < 0) {
        return BinderError("Cannot use negative values");
    }
    if (!impl_->ZeroFillNewImage(name, bytes)) {
        return BinderError("Failed to fill image with zeros");
    }
    return binder::Status::ok();
}

binder::Status ImageService::removeAllImages() {
    if (!CheckUid()) return UidSecurityError();

    std::lock_guard<std::mutex> guard(parent_->lock());
    if (!impl_->RemoveAllImages()) {
        return BinderError("Failed to remove all images");
    }
    return binder::Status::ok();
}

binder::Status ImageService::removeDisabledImages() {
    if (!CheckUid()) return UidSecurityError();

    std::lock_guard<std::mutex> guard(parent_->lock());
    if (!impl_->RemoveDisabledImages()) {
        return BinderError("Failed to remove disabled images");
    }
    return binder::Status::ok();
}

binder::Status ImageService::getMappedImageDevice(const std::string& name, std::string* device) {
    if (!CheckUid()) return UidSecurityError();

    std::lock_guard<std::mutex> guard(parent_->lock());
    if (!impl_->GetMappedImageDevice(name, device)) {
        *device = "";
    }
    return binder::Status::ok();
}

bool ImageService::CheckUid() {
    return uid_ == IPCThreadState::self()->getCallingUid();
}

binder::Status GsiService::openImageService(const std::string& prefix,
                                            android::sp<IImageService>* _aidl_return) {
    static constexpr char kImageMetadataPrefix[] = "/metadata/gsi/";
    static constexpr char kImageDataPrefix[] = "/data/gsi/";

    auto in_metadata_dir = kImageMetadataPrefix + prefix;
    auto in_data_dir = kImageDataPrefix + prefix;

    std::string metadata_dir, data_dir;
    if (!android::base::Realpath(in_metadata_dir, &metadata_dir)) {
        PLOG(ERROR) << "realpath failed: " << metadata_dir;
        return BinderError("Invalid path");
    }
    if (!android::base::Realpath(in_data_dir, &data_dir)) {
        PLOG(ERROR) << "realpath failed: " << data_dir;
        return BinderError("Invalid path");
    }
    if (!android::base::StartsWith(metadata_dir, kImageMetadataPrefix) ||
        !android::base::StartsWith(data_dir, kImageDataPrefix)) {
        return BinderError("Invalid path");
    }

    uid_t uid = IPCThreadState::self()->getCallingUid();
    if (uid != AID_ROOT) {
        return UidSecurityError();
    }

    auto impl = ImageManager::Open(metadata_dir, data_dir);
    if (!impl) {
        return BinderError("Unknown error");
    }

    *_aidl_return = new ImageService(this, std::move(impl), uid);
    return binder::Status::ok();
}

binder::Status GsiService::CheckUid(AccessLevel level) {
    std::vector<uid_t> allowed_uids{AID_ROOT, AID_SYSTEM};
    if (level == AccessLevel::SystemOrShell) {
        allowed_uids.push_back(AID_SHELL);
    }

    uid_t uid = IPCThreadState::self()->getCallingUid();
    for (const auto& allowed_uid : allowed_uids) {
        if (allowed_uid == uid) {
            return binder::Status::ok();
        }
    }
    return UidSecurityError();
}

static bool IsExternalStoragePath(const std::string& path) {
    if (!android::base::StartsWith(path, "/mnt/media_rw/")) {
        return false;
    }
    unique_fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd < 0) {
        PLOG(ERROR) << "open failed: " << path;
        return false;
    }
    struct statfs info;
    if (fstatfs(fd, &info)) {
        PLOG(ERROR) << "statfs failed: " << path;
        return false;
    }
    LOG(ERROR) << "fs type: " << info.f_type;
    return info.f_type == MSDOS_SUPER_MAGIC;
}

int GsiService::ValidateInstallParams(std::string& install_dir) {
    // If no install path was specified, use the default path. We also allow
    // specifying the top-level folder, and then we choose the correct location
    // underneath.
    if (install_dir.empty() || install_dir == "/data/gsi") {
        install_dir = kDefaultDsuImageFolder;
    }

    // Normalize the path and add a trailing slash.
    std::string origInstallDir = install_dir;
    if (!android::base::Realpath(origInstallDir, &install_dir)) {
        PLOG(ERROR) << "realpath failed: " << origInstallDir;
        return INSTALL_ERROR_GENERIC;
    }
    // Ensure the path ends in / for consistency.
    if (!android::base::EndsWith(install_dir, "/")) {
        install_dir += "/";
    }

    // Currently, we can only install to /data/gsi/ or external storage.
    if (IsExternalStoragePath(install_dir)) {
        Fstab fstab;
        if (!ReadDefaultFstab(&fstab)) {
            LOG(ERROR) << "cannot read default fstab";
            return INSTALL_ERROR_GENERIC;
        }
        FstabEntry* system = GetEntryForMountPoint(&fstab, "/system");
        if (!system) {
            LOG(ERROR) << "cannot find /system fstab entry";
            return INSTALL_ERROR_GENERIC;
        }
        if (fs_mgr_verity_is_check_at_most_once(*system)) {
            LOG(ERROR) << "cannot install GSIs to external media if verity uses check_at_most_once";
            return INSTALL_ERROR_GENERIC;
        }
    } else if (install_dir != kDefaultDsuImageFolder) {
        LOG(ERROR) << "cannot install DSU to " << install_dir;
        return INSTALL_ERROR_GENERIC;
    }
    return INSTALL_OK;
}

std::string GsiService::GetActiveInstalledImageDir() {
    // Just in case an install was left hanging.
    if (installer_) {
        return installer_->install_dir();
    } else {
        return GetInstalledImageDir();
    }
}

std::string GsiService::GetInstalledImageDir() {
    // If there's no install left, just return /data/gsi since that's where
    // installs go by default.
    std::string dir;
    if (android::base::ReadFileToString(kDsuInstallDirFile, &dir)) {
        return dir;
    }
    return kDefaultDsuImageFolder;
}

int GsiService::ReenableGsi(bool one_shot) {
    if (!android::gsi::IsGsiInstalled()) {
        LOG(ERROR) << "no gsi installed - cannot re-enable";
        return INSTALL_ERROR_GENERIC;
    }
    std::string boot_key;
    if (!GetInstallStatus(&boot_key)) {
        PLOG(ERROR) << "read " << kDsuInstallStatusFile;
        return INSTALL_ERROR_GENERIC;
    }
    if (boot_key != kInstallStatusDisabled) {
        LOG(ERROR) << "GSI is not currently disabled";
        return INSTALL_ERROR_GENERIC;
    }
    if (IsGsiRunning()) {
        if (!SetBootMode(one_shot) || !CreateInstallStatusFile()) {
            return IGsiService::INSTALL_ERROR_GENERIC;
        }
        return IGsiService::INSTALL_OK;
    }
    if (!SetBootMode(one_shot) || !CreateInstallStatusFile()) {
        return IGsiService::INSTALL_ERROR_GENERIC;
    }
    return IGsiService::INSTALL_OK;
}

bool GsiService::RemoveGsiFiles(const std::string& install_dir) {
    bool ok = true;
    if (auto manager = ImageManager::Open(kDsuMetadataDir, install_dir)) {
        std::vector<std::string> images = manager->GetAllBackingImages();
        for (auto&& image : images) {
            if (!android::base::EndsWith(image, "_gsi")) {
                continue;
            }
            if (manager->IsImageMapped(image)) {
                ok &= manager->UnmapImageDevice(image);
            }
            ok &= manager->DeleteBackingImage(image);
        }
    }
    std::vector<std::string> files{
            kDsuInstallStatusFile,
            kDsuOneShotBootFile,
            kDsuInstallDirFile,
            GetCompleteIndication(install_dir),
    };
    for (const auto& file : files) {
        std::string message;
        if (!RemoveFileIfExists(file, &message)) {
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
    if (installer_) {
        LOG(ERROR) << "cannot disable gsi during GSI installation";
        return false;
    }
    if (!DisableGsi()) {
        PLOG(ERROR) << "could not write gsi status";
        return false;
    }
    return true;
}

std::string GsiService::GetCompleteIndication(const std::string& installation) {
    auto strip_slash = installation.substr(0, installation.size() - 1);
    auto prefix = Split(strip_slash, "/").back();
    return "/metadata/gsi/" + prefix + "/complete";
}

bool GsiService::IsInstallationComplete(const std::string& install_dir) {
    std::string file = GetCompleteIndication(install_dir);
    std::string content;
    if (!ReadFileToString(file, &content)) {
        return false;
    }
    return content == "OK";
}

void GsiService::CleanCorruptedInstallation() {
    auto install_dir = GetInstalledImageDir();
    bool is_complete = IsInstallationComplete(install_dir);
    if (!is_complete) {
        if (!RemoveGsiFiles(install_dir)) {
            LOG(ERROR) << "Failed to CleanCorruptedInstallation on " << install_dir;
        }
    }
}

void GsiService::RunStartupTasks() {
    CleanCorruptedInstallation();

    std::string boot_key;
    if (!GetInstallStatus(&boot_key)) {
        PLOG(ERROR) << "read " << kDsuInstallStatusFile;
        return;
    }

    if (!IsGsiRunning()) {
        // Check if a wipe was requested from fastboot or adb-in-gsi.
        if (boot_key == kInstallStatusWipe) {
            RemoveGsiFiles(GetInstalledImageDir());
        }
    } else {
        // NB: When single-boot is enabled, init will write "disabled" into the
        // install_status file, which will cause GetBootAttempts to return
        // false. Thus, we won't write "ok" here.
        int ignore;
        if (GetBootAttempts(boot_key, &ignore)) {
            // Mark the GSI as having successfully booted.
            if (!android::base::WriteStringToFile(kInstallStatusOk, kDsuInstallStatusFile)) {
                PLOG(ERROR) << "write " << kDsuInstallStatusFile;
            }
        }
    }
}

}  // namespace gsi
}  // namespace android
