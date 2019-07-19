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
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <android-base/unique_fd.h>
#include <android/gsi/BnGsiService.h>
#include <binder/BinderService.h>
#include <libfiemap/split_fiemap_writer.h>
#include <liblp/builder.h>
#include "libgsi/libgsi.h"

#include "gsi_installer.h"

namespace android {
namespace gsi {

class GsiService : public BinderService<GsiService>, public BnGsiService {
  public:
    static void Register();

    GsiService();
    ~GsiService() override;

    binder::Status startGsiInstall(int64_t gsiSize, int64_t userdataSize, bool wipeUserdata,
                                   int* _aidl_return) override;
    binder::Status beginGsiInstall(const GsiInstallParams& params, int* _aidl_return) override;
    binder::Status commitGsiChunkFromStream(const ::android::os::ParcelFileDescriptor& stream,
                                            int64_t bytes, bool* _aidl_return) override;
    binder::Status getInstallProgress(::android::gsi::GsiProgress* _aidl_return) override;
    binder::Status commitGsiChunkFromMemory(const ::std::vector<uint8_t>& bytes,
                                            bool* _aidl_return) override;
    binder::Status cancelGsiInstall(bool* _aidl_return) override;
    binder::Status setGsiBootable(bool oneShot, int* _aidl_return) override;
    binder::Status isGsiEnabled(bool* _aidl_return) override;
    binder::Status removeGsiInstall(bool* _aidl_return) override;
    binder::Status disableGsiInstall(bool* _aidl_return) override;
    binder::Status isGsiRunning(bool* _aidl_return) override;
    binder::Status isGsiInstalled(bool* _aidl_return) override;
    binder::Status isGsiInstallInProgress(bool* _aidl_return) override;
    binder::Status getUserdataImageSize(int64_t* _aidl_return) override;
    binder::Status getGsiBootStatus(int* _aidl_return) override;
    binder::Status getInstalledGsiImageDir(std::string* _aidl_return) override;
    binder::Status wipeGsiUserdata(int* _aidl_return) override;
    binder::Status openImageManager(const std::string& prefix,
                                    android::sp<IImageManager>* _aidl_return) override;

    // This is in GsiService, rather than GsiInstaller, since we need to access
    // it outside of the main lock which protects the unique_ptr.
    void StartAsyncOperation(const std::string& step, int64_t total_bytes);
    void UpdateProgress(int status, int64_t bytes_processed);

    static char const* getServiceName() { return kGsiServiceName; }

    // Helper methods for GsiInstaller.
    static std::string GetImagePath(const std::string& image_dir, const std::string& name);
    static bool RemoveGsiFiles(const std::string& install_dir, bool wipeUserdata);
    bool should_abort() const { return should_abort_; }

    static void RunStartupTasks();

  private:
    friend class ImageManagerService;

    using LpMetadata = android::fs_mgr::LpMetadata;
    using MetadataBuilder = android::fs_mgr::MetadataBuilder;
    using SplitFiemap = android::fiemap::SplitFiemap;

    struct Image {
        std::unique_ptr<SplitFiemap> writer;
        uint64_t actual_size;
    };

    int ValidateInstallParams(GsiInstallParams* params);
    bool DisableGsiInstall();
    int ReenableGsi(bool one_shot);

    enum class AccessLevel { System, SystemOrShell };
    binder::Status CheckUid(AccessLevel level = AccessLevel::System);

    static std::string GetInstalledImagePath(const std::string& name);
    static std::string GetInstalledImageDir();

    std::mutex* lock() { return &lock_; }

    std::mutex lock_;
    std::unique_ptr<GsiInstaller> installer_;

    // These are initialized or set in StartInstall().
    std::atomic<bool> should_abort_ = false;

    // Progress bar state.
    std::mutex progress_lock_;
    GsiProgress progress_;
};

}  // namespace gsi
}  // namespace android
