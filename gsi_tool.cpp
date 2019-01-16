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

#include <getopt.h>
#include <stdio.h>
#include <sysexits.h>

#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <string>

#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <android/gsi/IGsiService.h>
#include <binder/IServiceManager.h>
#include <libgsi/libgsi.h>

using namespace android::gsi;
using namespace std::chrono_literals;

using android::sp;
using CommandCallback = std::function<int(sp<IGsiService>, int, char**)>;

static int Disable(sp<IGsiService> gsid, int argc, char** argv);
static int Enable(sp<IGsiService> gsid, int argc, char** argv);
static int Install(sp<IGsiService> gsid, int argc, char** argv);
static int Wipe(sp<IGsiService> gsid, int argc, char** argv);

static const std::map<std::string, CommandCallback> kCommandMap = {
        {"disable", Disable},
        {"enable", Enable},
        {"install", Install},
        {"wipe", Wipe},
};

static sp<IGsiService> getService() {
    static const int kSleepTimeMs = 50;
    static const int kTotalWaitTimeMs = 3000;
    for (int i = 0; i < kTotalWaitTimeMs / kSleepTimeMs; i++) {
        auto sm = android::defaultServiceManager();
        auto name = android::String16(kGsiServiceName);
        android::sp<android::IBinder> res = sm->checkService(name);
        if (res) {
            return android::interface_cast<IGsiService>(res);
        }
        usleep(kSleepTimeMs * 1000);
    }
    return nullptr;
}

static int Install(sp<IGsiService> gsid, int argc, char** argv) {
    struct option options[] = {
            {"gsi-size", required_argument, nullptr, 's'},
            {"userdata-size", required_argument, nullptr, 'u'},
            {"wipe", no_argument, nullptr, 'w'},
            {nullptr, 0, nullptr, 0},
    };

    int64_t gsi_size = 0;
    int64_t userdata_size = 0;
    bool wipe_userdata = false;

    int rv, index;
    while ((rv = getopt_long_only(argc, argv, "", options, &index)) != -1) {
        switch (rv) {
            case 's':
                if (!android::base::ParseInt(optarg, &gsi_size) || gsi_size <= 0) {
                    std::cout << "Could not parse image size: " << optarg << std::endl;
                    return EX_USAGE;
                }
                break;
            case 'u':
                if (!android::base::ParseInt(optarg, &userdata_size) || userdata_size < 0) {
                    std::cout << "Could not parse image size: " << optarg << std::endl;
                    return EX_USAGE;
                }
                break;
            case 'w':
                wipe_userdata = true;
                break;
        }
    }

    if (gsi_size <= 0) {
        std::cout << "Must specify --gsi-size." << std::endl;
        return EX_USAGE;
    }

    android::base::unique_fd input(dup(1));
    if (input < 0) {
        std::cout << "Error duplicating descriptor: " << strerror(errno);
        return EX_SOFTWARE;
    }

    bool ok;
    auto status = gsid->startGsiInstall(gsi_size, userdata_size, wipe_userdata, &ok);
    if (!status.isOk() || !ok) {
        std::cout << "Could not start live image install";
        return EX_SOFTWARE;
    }

    android::os::ParcelFileDescriptor stream(std::move(input));

    status = gsid->commitGsiChunkFromStream(stream, gsi_size, &ok);
    if (!status.isOk() || !ok) {
        std::cout << "Could not commit live image data";
        return EX_SOFTWARE;
    }

    status = gsid->setGsiBootable(&ok);
    if (!status.isOk() || !ok) {
        std::cout << "Could not make live image bootable";
        return EX_SOFTWARE;
    }
    return 0;
}

static int Wipe(sp<IGsiService> gsid, int argc, char** /* argv */) {
    if (argc > 1) {
        std::cout << "Unrecognized arguments to wipe." << std::endl;
        return EX_USAGE;
    }
    bool ok;
    auto status = gsid->removeGsiInstall(&ok);
    if (!status.isOk() || !ok) {
        std::cout << status.exceptionMessage().string() << std::endl;
        return EX_SOFTWARE;
    }
    std::cout << "Live image install successfully removed." << std::endl;
    return 0;
}

static int Enable(sp<IGsiService> gsid, int argc, char** /* argv */) {
    if (argc > 1) {
        std::cout << "Unrecognized arguments to enable." << std::endl;
        return EX_USAGE;
    }

    bool installed = false;
    gsid->isGsiInstalled(&installed);
    if (!installed) {
        std::cout << "Could not find GSI install to re-enable" << std::endl;
        return EX_SOFTWARE;
    }

    bool installing = false;
    gsid->isGsiInstallInProgress(&installing);
    if (installing) {
        std::cout << "Cannot enable or disable while an installation is in progress." << std::endl;
        return EX_SOFTWARE;
    }

    bool ok = false;
    gsid->setGsiBootable(&ok);
    if (!ok) {
        std::cout << "Error re-enabling GSI" << std::endl;
        return EX_SOFTWARE;
    }
    std::cout << "Live image install successfully enabled." << std::endl;
    return 0;
}

static int Disable(sp<IGsiService> gsid, int argc, char** /* argv */) {
    if (argc > 1) {
        std::cout << "Unrecognized arguments to disable." << std::endl;
        return EX_USAGE;
    }

    bool installing = false;
    gsid->isGsiInstallInProgress(&installing);
    if (installing) {
        std::cout << "Cannot enable or disable while an installation is in progress." << std::endl;
        return EX_SOFTWARE;
    }

    bool ok = false;
    gsid->disableGsiInstall(&ok);
    if (!ok) {
        std::cout << "Error disabling GSI" << std::endl;
        return EX_SOFTWARE;
    }
    std::cout << "Live image install successfully disabled." << std::endl;
    return 0;
}

static int usage(int /* argc */, char* argv[]) {
    fprintf(stderr,
            "%s - command-line tool for installing GSI images.\n"
            "\n"
            "Usage:\n"
            "  %s <disable|install|wipe> [options]\n"
            "\n"
            "  disable      Disable the currently installed GSI.\n"
            "  enable       Enable a previously disabled GSI.\n"
            "  install      Install a new GSI. Specify the image size with\n"
            "               --gsi-size and the desired userdata size with\n"
            "               --userdata-size (the latter defaults to 8GiB)\n"
            "               --wipe (remove old gsi userdata first)\n"
            "  wipe         Completely remove a GSI and its associated data\n",
            argv[0], argv[0]);
    return EX_USAGE;
}

int main(int argc, char** argv) {
    // Ensure gsid is started.
    android::base::SetProperty("ctl.start", "gsid");
    if (!android::base::WaitForProperty("init.svc.gsid", "running", 5s)) {
        std::cout << "Unable to start gsid";
        return EX_SOFTWARE;
    }

    auto gsid = getService();
    if (!gsid) {
        std::cout << "Could not connect to the gsid service." << std::endl;
        return EX_NOPERM;
    }

    if (1 >= argc) {
        std::cout << "Expected command." << std::endl;
        return EX_USAGE;
    }

    std::string command = argv[1];
    auto iter = kCommandMap.find(command);
    if (iter == kCommandMap.end()) {
        std::cout << "Unrecognized command: " << command << std::endl;
        return usage(argc, argv);
    }

    int rc = iter->second(gsid, argc - 1, argv + 1);

    android::base::SetProperty("ctl.stop", "gsid");
    return rc;
}
