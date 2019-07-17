
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

#include "utility.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "file_paths.h"

namespace android {
namespace gsi {

using namespace android::fiemap;

// Given a SplitFiemap, this returns a device path that will work during first-
// stage init (i.e., its path can be found by InitRequiredDevices).
std::string GetDevicePathForFile(SplitFiemap* file) {
    auto bdev_path = file->bdev_path();

    struct stat userdata, given;
    if (!stat(bdev_path.c_str(), &given) && !stat(kUserdataDevice, &userdata)) {
        if (S_ISBLK(given.st_mode) && S_ISBLK(userdata.st_mode) &&
            given.st_rdev == userdata.st_rdev) {
            return kUserdataDevice;
        }
    }
    return bdev_path;
}

}  // namespace gsi
}  // namespace android
