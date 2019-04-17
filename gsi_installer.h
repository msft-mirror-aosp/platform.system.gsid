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

#include <memory>
#include <string>

#include <android/gsi/IGsiService.h>
#include <libfiemap_writer/split_fiemap_writer.h>
#include <liblp/builder.h>

namespace android {
namespace gsi {

class GsiService;

class GsiInstaller final {
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
    int SetGsiBootable(bool one_shot);

    // Methods for re-enabling an existing install.
    int ReenableGsi(bool one_shot);

    // Clean up install state if gsid crashed and restarted.
    static void PostInstallCleanup();

    // This helper class will redirect writes to either a SplitFiemap or
    // device-mapper.
    class WriteHelper {
      public:
        virtual ~WriteHelper(){};
        virtual bool Write(const void* data, uint64_t bytes) = 0;
        virtual bool Flush() = 0;

        WriteHelper() = default;
        WriteHelper(const WriteHelper&) = delete;
        WriteHelper& operator=(const WriteHelper&) = delete;
        WriteHelper& operator=(WriteHelper&&) = delete;
        WriteHelper(WriteHelper&&) = delete;
    };

    const std::string& install_dir() const { return install_dir_; }
    uint64_t userdata_size() const { return userdata_size_; }

  private:
    using MetadataBuilder = android::fs_mgr::MetadataBuilder;
    using LpMetadata = android::fs_mgr::LpMetadata;
    using SplitFiemap = android::fiemap_writer::SplitFiemap;

    // The image file may be larger than the requested size, due to alignment,
    // so we must track the requested size as well.
    struct Image {
        std::unique_ptr<SplitFiemap> writer;
        uint64_t actual_size;
    };

    int PerformSanityChecks();
    int PreallocateFiles();
    int PreallocateUserdata();
    int PreallocateSystem();
    int DetermineReadWriteMethod();
    bool FormatUserdata();
    bool AddPartitionFiemap(MetadataBuilder* builder, android::fs_mgr::Partition* partition,
                            const Image& image, const std::string& block_device);
    std::unique_ptr<LpMetadata> CreateMetadata();
    std::unique_ptr<SplitFiemap> CreateFiemapWriter(const std::string& path, uint64_t size,
                                                    int* error);
    std::unique_ptr<WriteHelper> OpenPartition(const std::string& name);
    int GetExistingImage(const LpMetadata& metadata, const std::string& name, Image* image);
    bool CreateInstallStatusFile();
    bool CreateMetadataFile();
    bool SetBootMode(bool one_shot);
    std::string GetImagePath(const std::string& name);

    GsiService* service_;

    std::string install_dir_;
    std::string userdata_gsi_path_;
    std::string system_gsi_path_;
    uint64_t userdata_block_size_ = 0;
    uint64_t system_block_size_ = 0;
    uint64_t gsi_size_ = 0;
    uint64_t userdata_size_ = 0;
    bool can_use_devicemapper_ = false;
    bool wipe_userdata_ = false;
    bool wipe_userdata_on_failure_ = false;
    // Remaining data we're waiting to receive for the GSI image.
    uint64_t gsi_bytes_written_ = 0;
    bool succeeded_ = false;

    std::unique_ptr<WriteHelper> system_writer_;

    // This is used to track which GSI partitions have been created.
    std::map<std::string, Image> partitions_;
    std::unique_ptr<LpMetadata> metadata_;
};

}  // namespace gsi
}  // namespace android
