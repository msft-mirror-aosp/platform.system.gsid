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

#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

#include <android-base/unique_fd.h>
#include <android/gsi/BnGsiService.h>
#include <binder/BinderService.h>
#include <libfiemap_writer/fiemap_writer.h>
#include <liblp/builder.h>
#include "libgsi/libgsi.h"

namespace android {
namespace gsi {

class GsiService : public BinderService<GsiService>, public BnGsiService {
  public:
    static void Register();

    GsiService();
    ~GsiService() override;

    binder::Status startGsiInstall(int64_t gsiSize, int64_t userdataSize, bool wipeUserdata,
                                   int* _aidl_return) override;
    binder::Status commitGsiChunkFromStream(const ::android::os::ParcelFileDescriptor& stream,
                                            int64_t bytes, bool* _aidl_return) override;
    binder::Status getInstallProgress(::android::gsi::GsiProgress* _aidl_return) override;
    binder::Status commitGsiChunkFromMemory(const ::std::vector<uint8_t>& bytes,
                                            bool* _aidl_return) override;
    binder::Status cancelGsiInstall(bool* _aidl_return) override;
    binder::Status setGsiBootable(int* _aidl_return) override;
    binder::Status removeGsiInstall(bool* _aidl_return) override;
    binder::Status disableGsiInstall(bool* _aidl_return) override;
    binder::Status isGsiRunning(bool* _aidl_return) override;
    binder::Status isGsiInstalled(bool* _aidl_return) override;
    binder::Status isGsiInstallInProgress(bool* _aidl_return) override;
    binder::Status getUserdataImageSize(int64_t* _aidl_return) override;

    static char const* getServiceName() { return kGsiServiceName; }

    static void RunStartupTasks();

  private:
    using LpMetadata = android::fs_mgr::LpMetadata;
    using MetadataBuilder = android::fs_mgr::MetadataBuilder;

    int StartInstall(int64_t gsi_size, int64_t userdata_size, bool wipe_userdata);
    int PerformSanityChecks();
    int PreallocateFiles();
    bool FormatUserdata();
    bool CommitGsiChunk(int stream_fd, int64_t bytes);
    bool CommitGsiChunk(const void* data, size_t bytes);
    bool SetGsiBootable();
    int ReenableGsi();
    bool DisableGsiInstall();
    bool EnsureFolderExists(const std::string& path);
    bool AddPartitionFiemap(android::fs_mgr::MetadataBuilder* builder,
                            android::fs_mgr::Partition* partition,
                            android::fiemap_writer::FiemapWriter* writer);
    std::unique_ptr<LpMetadata> CreateMetadata(android::fiemap_writer::FiemapWriter* userdata,
                                               android::fiemap_writer::FiemapWriter* system);
    fiemap_writer::FiemapUniquePtr CreateFiemapWriter(const std::string& path, uint64_t size,
                                                      int* error);
    bool CreateInstallStatusFile();
    bool CreateMetadataFile(const android::fs_mgr::LpMetadata& metadata);
    void PostInstallCleanup();

    void StartAsyncOperation(const std::string& step, int64_t total_bytes);
    void UpdateProgress(int status, int64_t bytes_processed);

    static bool RemoveGsiFiles(bool wipeUserdata);

    std::mutex main_lock_;
    bool installing_ = false;
    uint64_t userdata_block_size_;
    uint64_t system_block_size_;
    uint64_t gsi_size_;
    uint64_t userdata_size_;
    bool wipe_userdata_;
    bool wipe_userdata_on_failure_;
    // Remaining data we're waiting to receive for the GSI image.
    uint64_t gsi_bytes_written_;

    // Progress bar state.
    std::mutex progress_lock_;
    GsiProgress progress_;

    android::base::unique_fd system_fd_;

    std::unique_ptr<LpMetadata> metadata_;
};

}  // namespace gsi
}  // namespace android
