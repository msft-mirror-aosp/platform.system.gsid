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
#include <condition_variable>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <android/gsi/IGsiService.h>
#include <binder/IServiceManager.h>
#include <cutils/android_reboot.h>
#include <libgsi/libgsi.h>

using namespace android::gsi;
using namespace std::chrono_literals;

using android::sp;
using CommandCallback = std::function<int(sp<IGsiService>, int, char**)>;

static int Disable(sp<IGsiService> gsid, int argc, char** argv);
static int Enable(sp<IGsiService> gsid, int argc, char** argv);
static int Install(sp<IGsiService> gsid, int argc, char** argv);
static int Wipe(sp<IGsiService> gsid, int argc, char** argv);
static int Status(sp<IGsiService> gsid, int argc, char** argv);

static const std::map<std::string, CommandCallback> kCommandMap = {
        {"disable", Disable},
        {"enable", Enable},
        {"install", Install},
        {"wipe", Wipe},
        {"status", Status},
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

class ProgressBar {
  public:
    explicit ProgressBar(sp<IGsiService> gsid) : gsid_(gsid) {}

    ~ProgressBar() { Stop(); }

    void Display() {
        Finish();
        done_ = false;
        last_update_ = {};
        worker_ = std::make_unique<std::thread>([this]() { Worker(); });
    }

    void Stop() {
        if (!worker_) {
            return;
        }
        SignalDone();
        worker_->join();
        worker_ = nullptr;
    }

    void Finish() {
        if (!worker_) {
            return;
        }
        Stop();
        FinishLastBar();
    }

  private:
    void Worker() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!done_) {
            if (!UpdateProgress()) {
                return;
            }
            cv_.wait_for(lock, 500ms, [this] { return done_; });
        }
    }

    bool UpdateProgress() {
        GsiProgress latest;
        auto status = gsid_->getInstallProgress(&latest);
        if (!status.isOk()) {
            std::cout << std::endl;
            return false;
        }
        if (latest.status == IGsiService::STATUS_NO_OPERATION) {
            return true;
        }
        if (last_update_.step != latest.step) {
            FinishLastBar();
        }
        Display(latest);
        return true;
    }

    void FinishLastBar() {
        // If no bar was in progress, don't do anything.
        if (last_update_.total_bytes == 0) {
            return;
        }
        // Ensure we finish the display at 100%.
        last_update_.bytes_processed = last_update_.total_bytes;
        Display(last_update_);
        std::cout << std::endl;
    }

    void Display(const GsiProgress& progress) {
        if (progress.total_bytes == 0) {
            return;
        }

        static constexpr int kColumns = 80;
        static constexpr char kRedColor[] = "\x1b[31m";
        static constexpr char kGreenColor[] = "\x1b[32m";
        static constexpr char kResetColor[] = "\x1b[0m";

        int percentage = (progress.bytes_processed * 100) / progress.total_bytes;
        int64_t bytes_per_col = progress.total_bytes / kColumns;
        uint32_t fill_count = progress.bytes_processed / bytes_per_col;
        uint32_t dash_count = kColumns - fill_count;
        std::string fills = std::string(fill_count, '=');
        std::string dashes = std::string(dash_count, '-');

        // Give the end of the bar some flare.
        if (!fills.empty() && !dashes.empty()) {
            fills[fills.size() - 1] = '>';
        }

        fprintf(stdout, "\r%-15s%6d%% ", progress.step.c_str(), percentage);
        fprintf(stdout, "%s[%s%s%s", kGreenColor, fills.c_str(), kRedColor, dashes.c_str());
        fprintf(stdout, "%s]%s", kGreenColor, kResetColor);
        fflush(stdout);

        last_update_ = progress;
    }

    void SignalDone() {
        std::lock_guard<std::mutex> guard(mutex_);
        done_ = true;
        cv_.notify_all();
    }

  private:
    sp<IGsiService> gsid_;
    std::unique_ptr<std::thread> worker_;
    std::condition_variable cv_;
    std::mutex mutex_;
    GsiProgress last_update_;
    bool done_ = false;
};

static int Install(sp<IGsiService> gsid, int argc, char** argv) {
    struct option options[] = {
            {"gsi-size", required_argument, nullptr, 's'},
            {"no-reboot", no_argument, nullptr, 'n'},
            {"userdata-size", required_argument, nullptr, 'u'},
            {"wipe", no_argument, nullptr, 'w'},
            {nullptr, 0, nullptr, 0},
    };

    int64_t gsi_size = 0;
    int64_t userdata_size = 0;
    bool wipe_userdata = false;
    bool reboot = true;

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
            case 'n':
                reboot = false;
                break;
        }
    }

    if (gsi_size <= 0) {
        std::cout << "Must specify --gsi-size." << std::endl;
        return EX_USAGE;
    }

    bool running_gsi = false;
    gsid->isGsiRunning(&running_gsi);
    if (running_gsi) {
        std::cout << "Cannot install a GSI within a live GSI." << std::endl;
        std::cout << "Use gsi_tool disable or wipe and reboot first." << std::endl;
        return EX_SOFTWARE;
    }

    android::base::unique_fd input(dup(1));
    if (input < 0) {
        std::cout << "Error duplicating descriptor: " << strerror(errno) << std::endl;
        return EX_SOFTWARE;
    }

    // Note: the progress bar needs to be re-started in between each call.
    ProgressBar progress(gsid);
    progress.Display();

    int error;
    auto status = gsid->startGsiInstall(gsi_size, userdata_size, wipe_userdata, &error);
    if (!status.isOk() || error != IGsiService::INSTALL_OK) {
        std::cout << "Could not start live image install, error code " << error << std::endl;
        return EX_SOFTWARE;
    }

    android::os::ParcelFileDescriptor stream(std::move(input));

    bool ok = false;
    progress.Display();
    gsid->commitGsiChunkFromStream(stream, gsi_size, &ok);
    if (!ok) {
        std::cout << "Could not commit live image data" << std::endl;
        return EX_SOFTWARE;
    }

    progress.Finish();

    status = gsid->setGsiBootable(&error);
    if (!status.isOk() || error != IGsiService::INSTALL_OK) {
        std::cout << "Could not make live image bootable, error code " << error << std::endl;
        return EX_SOFTWARE;
    }

    if (reboot) {
        if (!android::base::SetProperty(ANDROID_RB_PROPERTY, "reboot,adb")) {
            std::cout << "Failed to reboot automatically" << std::endl;
            return EX_SOFTWARE;
        }
    } else {
        std::cout << "Please reboot to use the GSI." << std::endl;
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

static int Status(sp<IGsiService> gsid, int argc, char** /* argv */) {
    if (argc > 1) {
        std::cout << "Unrecognized arguments to status." << std::endl;
        return EX_USAGE;
    }
    bool running;
    auto status = gsid->isGsiRunning(&running);
    if (!status.isOk()) {
        std::cout << status.exceptionMessage().string() << std::endl;
        return EX_SOFTWARE;
    } else if (running) {
        std::cout << "running" << std::endl;
        return 0;
    }
    bool installed;
    status = gsid->isGsiInstalled(&installed);
    if (!status.isOk()) {
        std::cout << status.exceptionMessage().string() << std::endl;
        return EX_SOFTWARE;
    } else if (installed) {
        std::cout << "installed" << std::endl;
        return 0;
    }
    std::cout << "normal" << std::endl;
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

    int error;
    auto status = gsid->setGsiBootable(&error);
    if (!status.isOk() || error != IGsiService::INSTALL_OK) {
        std::cout << "Error re-enabling GSI, error code " << error << std::endl;
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
            "  %s <disable|install|wipe|status> [options]\n"
            "\n"
            "  disable      Disable the currently installed GSI.\n"
            "  enable       Enable a previously disabled GSI.\n"
            "  install      Install a new GSI. Specify the image size with\n"
            "               --gsi-size and the desired userdata size with\n"
            "               --userdata-size (the latter defaults to 8GiB)\n"
            "               --wipe (remove old gsi userdata first)\n"
            "  wipe         Completely remove a GSI and its associated data\n"
            "  status       Show status",
            argv[0], argv[0]);
    return EX_USAGE;
}

int main(int argc, char** argv) {
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

    if (command != "status") {
        // Installing or changing the GSI needs root.
        if (getuid() != 0) {
            std::cout << argv[0] << " must be run as root." << std::endl;
            return EX_NOPERM;
        }
    }

    auto iter = kCommandMap.find(command);
    if (iter == kCommandMap.end()) {
        std::cout << "Unrecognized command: " << command << std::endl;
        return usage(argc, argv);
    }

    int rc = iter->second(gsid, argc - 1, argv + 1);
    return rc;
}
