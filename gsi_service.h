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
#include <android/gsi/BnGsid.h>
#include <binder/BinderService.h>
#include <libfiemap/split_fiemap_writer.h>
#include <liblp/builder.h>
#include "libgsi/libgsi.h"

#include "partition_installer.h"

namespace android {
namespace gsi {

class Gsid : public BinderService<Gsid>, public BnGsid {
  public:
    static void Register();
    static char const* getServiceName() { return kGsiServiceName; }

    binder::Status getClient(android::sp<IGsiService>* _aidl_return) override;

  private:
    friend class GsiService;
    friend class ImageService;

    std::mutex& lock() { return lock_; }

    std::mutex lock_;
};

class GsiService : public BinderService<GsiService>, public BnGsiService {
  public:
    ~GsiService() override;

    static android::sp<IGsiService> Get(Gsid* parent);

    binder::Status openInstall(const std::string& install_dir, int* _aidl_return) override;
    binder::Status closeInstall(int32_t* _aidl_return) override;
    binder::Status createPartition(const ::std::string& name, int64_t size, bool readOnly,
                                   int32_t* _aidl_return) override;
    binder::Status commitGsiChunkFromStream(const ::android::os::ParcelFileDescriptor& stream,
                                            int64_t bytes, bool* _aidl_return) override;
    binder::Status getInstallProgress(::android::gsi::GsiProgress* _aidl_return) override;
    binder::Status setGsiAshmem(const ::android::os::ParcelFileDescriptor& ashmem, int64_t size,
                                bool* _aidl_return) override;
    binder::Status commitGsiChunkFromAshmem(int64_t bytes, bool* _aidl_return) override;
    binder::Status cancelGsiInstall(bool* _aidl_return) override;
    binder::Status enableGsi(bool oneShot, int* _aidl_return) override;
    binder::Status isGsiEnabled(bool* _aidl_return) override;
    binder::Status removeGsi(bool* _aidl_return) override;
    binder::Status disableGsi(bool* _aidl_return) override;
    binder::Status isGsiInstalled(bool* _aidl_return) override;
    binder::Status isGsiRunning(bool* _aidl_return) override;
    binder::Status isGsiInstallInProgress(bool* _aidl_return) override;
    binder::Status getInstalledGsiImageDir(std::string* _aidl_return) override;
    binder::Status zeroPartition(const std::string& name, int* _aidl_return) override;
    binder::Status openImageService(const std::string& prefix,
                                    android::sp<IImageService>* _aidl_return) override;
    binder::Status dumpDeviceMapperDevices(std::string* _aidl_return) override;

    // This is in GsiService, rather than GsiInstaller, since we need to access
    // it outside of the main lock which protects the unique_ptr.
    void StartAsyncOperation(const std::string& step, int64_t total_bytes);
    void UpdateProgress(int status, int64_t bytes_processed);

    // Helper methods for GsiInstaller.
    static bool RemoveGsiFiles(const std::string& install_dir);
    bool should_abort() const { return should_abort_; }
    Gsid* parent() const { return parent_.get(); }

    static void RunStartupTasks();
    static std::string GetInstalledImageDir();
    std::string GetActiveInstalledImageDir();

  private:
    GsiService(Gsid* parent);
    static int ValidateInstallParams(std::string& install_dir);
    bool DisableGsiInstall();
    int ReenableGsi(bool one_shot);
    static void CleanCorruptedInstallation();
    static int SaveInstallation(const std::string&);
    static bool IsInstallationComplete(const std::string&);
    static std::string GetCompleteIndication(const std::string&);

    enum class AccessLevel { System, SystemOrShell };
    binder::Status CheckUid(AccessLevel level = AccessLevel::System);
    bool CreateInstallStatusFile();
    bool SetBootMode(bool one_shot);

    static android::wp<GsiService> sInstance;

    std::string install_dir_ = {};
    android::sp<Gsid> parent_;
    std::unique_ptr<PartitionInstaller> installer_;

    // These are initialized or set in StartInstall().
    std::atomic<bool> should_abort_ = false;

    // Progress bar state.
    std::mutex progress_lock_;
    GsiProgress progress_;
};

}  // namespace gsi
}  // namespace android
