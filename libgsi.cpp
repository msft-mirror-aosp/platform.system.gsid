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

#include "libgsi/libgsi.h"

#include <unistd.h>

#include <android-base/file.h>

#include "file_paths.h"

namespace android {
namespace gsi {

bool IsGsiRunning() {
    return !access(kGsiBootedIndicatorFile, F_OK);
}

bool IsGsiInstalled() {
    return !access(kGsiBootableFile, F_OK);
}

static bool CanBootIntoGsi(std::string* error) {
    if (!IsGsiInstalled()) {
        *error = "not detected";
        return false;
    }
    // :TODO: boot attempts
    return true;
}

bool CanBootIntoGsi(std::string* metadata_file, std::string* error) {
    // Always delete this as a safety precaution, so we can return to the
    // original system image. If we're confident GSI will boot, this will
    // get re-created by MarkSystemAsGsi.
    android::base::RemoveFileIfExists(kGsiBootedIndicatorFile);

    if (!CanBootIntoGsi(error)) {
        android::base::RemoveFileIfExists(kGsiBootableFile);
        return false;
    }

    *metadata_file = kGsiMetadata;
    return true;
}

bool UninstallGsi() {
    if (!android::base::RemoveFileIfExists(kGsiBootableFile)) {
        return false;
    }
    return true;
}

bool MarkSystemAsGsi() {
    return android::base::WriteStringToFile("1", kGsiBootedIndicatorFile);
}

}  // namespace gsi
}  // namespace android
