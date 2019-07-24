//
// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once

#include <stdint.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <android-base/unique_fd.h>

namespace android {
namespace fiemap {

class ImageManager final {
  public:
    // Return an ImageManager for the given metadata and data directories. Both
    // directories must already exist.
    static std::unique_ptr<ImageManager> Open(const std::string& metadata_dir,
                                              const std::string& data_dir);

    // Create an image that can be mapped as a block-device.
    bool CreateBackingImage(const std::string& name, uint64_t size, bool readonly,
                            std::function<bool(uint64_t, uint64_t)>&& on_progress);

    // Delete an image created with CreateBackingImage. Its entry will be
    // removed from the associated lp_metadata file.
    bool DeleteBackingImage(const std::string& name);

    // Create a block device for an image previously created with
    // CreateBackingImage. This will wait for at most |timeout_ms| milliseconds
    // for |path| to be available, and will return false if not available in
    // the requested time. If |timeout_ms| is zero, this is NOT guaranteed to
    // return true. A timeout of 10s is recommended.
    bool MapImageDevice(const std::string& name, const std::chrono::milliseconds& timeout_ms,
                        std::string* path);

    // Unmap a block device previously mapped with mapBackingImage.
    bool UnmapImageDevice(const std::string& name);

    // Returns true if the specified image is mapped to a device.
    bool IsImageMapped(const std::string& name);

    // Find and remove all images and metadata for a given image dir.
    bool RemoveAllImages();

    // Returns true whether the named backing image exists.
    bool BackingImageExists(const std::string& name);

    // Returns true if the named partition exists. This does not check the
    // consistency of the backing image/data file.
    bool PartitionExists(const std::string& name);

    // Validates that all images still have pinned extents. This will be removed
    // once b/134588268 is fixed.
    bool Validate();

  private:
    ImageManager(const std::string& metadata_dir, const std::string& data_dir);
    std::string GetImageHeaderPath(const std::string& name);
    std::string GetStatusFilePath(const std::string& image_name);
    bool MapWithLoopDevice(const std::string& name, const std::chrono::milliseconds& timeout_ms,
                           std::string* path);
    bool MapWithLoopDeviceList(const std::vector<std::string>& device_list, const std::string& name,
                               const std::chrono::milliseconds& timeout_ms, std::string* path);
    bool MapWithDmLinear(const std::string& name, const std::string& block_device,
                         const std::chrono::milliseconds& timeout_ms, std::string* path);
    bool UnmapImageDevice(const std::string& name, bool force);

    ImageManager(const ImageManager&) = delete;
    ImageManager& operator=(const ImageManager&) = delete;
    ImageManager& operator=(ImageManager&&) = delete;
    ImageManager(ImageManager&&) = delete;

    std::string metadata_dir_;
    std::string data_dir_;
};

// RAII helper class for mapping and opening devices with an ImageManager.
class MappedDevice final {
  public:
    static std::unique_ptr<MappedDevice> Open(ImageManager* manager,
                                              const std::chrono::milliseconds& timeout_ms,
                                              const std::string& name);

    ~MappedDevice();

    int fd() const { return fd_; }

  protected:
    MappedDevice(ImageManager* manager, const std::string& name, const std::string& path);

    ImageManager* manager_;
    std::string name_;
    android::base::unique_fd fd_;
};

}  // namespace fiemap
}  // namespace android
