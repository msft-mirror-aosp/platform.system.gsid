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

#include <stdint.h>
#include <sys/mman.h>

#include <memory>
#include <string>

#include <android-base/unique_fd.h>
#include <android/gsi/IGsiService.h>
#include <android/gsi/MappedImage.h>
#include <libfiemap/image_manager.h>
#include <liblp/builder.h>

namespace android {
namespace gsi {

class GsiService;

class GsiInstaller final {
    using ImageManager = android::fiemap::ImageManager;
    using MappedDevice = android::fiemap::MappedDevice;

  public:
    // Constructor for a new GSI installation.
    GsiInstaller(GsiService* service, const GsiInstallParams& params);
    // Constructor for re-enabling a previous GSI installation.
    GsiInstaller(GsiService* service, const std::string& install_dir);
    ~GsiInstaller();

    // Methods for a clean GSI install.
    int StartInstall();
    bool CommitGsiChunk(int stream_fd, int64_t bytes);
    bool CommitGsiChunk(const void* data, size_t bytes);
    bool MapAshmem(int fd, size_t size);
    bool CommitGsiChunk(size_t bytes);
    int SetGsiBootable(bool one_shot);

    // Methods for interacting with an existing install.
    int ReenableGsi(bool one_shot);
    int WipeUserdata();

    // Clean up install state if gsid crashed and restarted.
    static void PostInstallCleanup();
    static void PostInstallCleanup(ImageManager* manager);

    const std::string& install_dir() const { return install_dir_; }
    uint64_t userdata_size() const { return userdata_size_; }

  private:
    int PerformSanityChecks();
    int PreallocateFiles();
    int PreallocateUserdata();
    int PreallocateSystem();
    bool FormatUserdata();
    bool CreateImage(const std::string& name, uint64_t size, bool readonly);
    std::unique_ptr<MappedDevice> OpenPartition(const std::string& name);
    int CheckInstallState();
    bool CreateInstallStatusFile();
    bool SetBootMode(bool one_shot);
    bool IsFinishedWriting();
    bool IsAshmemMapped();
    void UnmapAshmem();

    GsiService* service_;

    std::string install_dir_;
    std::unique_ptr<ImageManager> images_;
    uint64_t gsi_size_ = 0;
    uint64_t userdata_size_ = 0;
    bool wipe_userdata_ = false;
    bool wipe_userdata_on_failure_ = false;
    // Remaining data we're waiting to receive for the GSI image.
    uint64_t gsi_bytes_written_ = 0;
    bool succeeded_ = false;
    uint64_t ashmem_size_ = -1;
    void* ashmem_data_ = MAP_FAILED;

    std::unique_ptr<MappedDevice> system_device_;
};

}  // namespace gsi
}  // namespace android
