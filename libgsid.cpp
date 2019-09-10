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

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/gsi/IGsiService.h>
#include <android/gsi/IGsid.h>
#include <binder/IServiceManager.h>
#include <libgsi/libgsi.h>

namespace android {
namespace gsi {

using namespace std::chrono_literals;
using android::sp;

static sp<IGsid> GetGsid() {
    if (android::base::GetProperty("init.svc.gsid", "") != "running") {
        if (!android::base::SetProperty("ctl.start", "gsid") ||
            !android::base::WaitForProperty("init.svc.gsid", "running", 5s)) {
            LOG(ERROR) << "Unable to start gsid";
            return nullptr;
        }
    }

    static const int kSleepTimeMs = 50;
    static const int kTotalWaitTimeMs = 3000;
    for (int i = 0; i < kTotalWaitTimeMs / kSleepTimeMs; i++) {
        auto sm = android::defaultServiceManager();
        auto name = android::String16(kGsiServiceName);
        android::sp<android::IBinder> res = sm->checkService(name);
        if (res) {
            return android::interface_cast<IGsid>(res);
        }
        usleep(kSleepTimeMs * 1000);
    }

    LOG(ERROR) << "Timed out trying to start gsid";
    return nullptr;
}

sp<IGsiService> GetGsiService() {
    auto gsid = GetGsid();
    if (!gsid) {
        return nullptr;
    }

    sp<IGsiService> service;
    auto status = gsid->getClient(&service);
    if (!status.isOk() || !service) {
        LOG(ERROR) << "Error acquiring IGsid: " << status.exceptionMessage().string();
        return nullptr;
    }
    return service;
}

}  // namespace gsi
}  // namespace android
