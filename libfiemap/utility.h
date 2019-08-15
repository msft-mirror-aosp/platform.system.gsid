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

#include <string>

#include <libfiemap/split_fiemap_writer.h>

namespace android {
namespace fiemap {

// Given a file that will be created, determine the maximum size its containing
// filesystem allows. Note this is a theoretical maximum size; free space is
// ignored entirely.
uint64_t DetermineMaximumFileSize(const std::string& file_path);

// Given a SplitFiemap, this returns a device path that will work during first-
// stage init (i.e., its path can be found by InitRequiredDevices).
std::string GetDevicePathForFile(android::fiemap::SplitFiemap* file);

// Combine two path components into a single path.
std::string JoinPaths(const std::string& dir, const std::string& file);

}  // namespace fiemap
}  // namespace android
