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

package com.android.tests.dsu;

import com.android.tradefed.build.BuildRetrievalError;
import com.android.tradefed.build.IBuildInfo;
import com.android.tradefed.build.IDeviceBuildInfo;
import com.android.tradefed.config.Option;
import com.android.tradefed.config.Option.Importance;
import com.android.tradefed.device.DeviceNotAvailableException;
import com.android.tradefed.testtype.DeviceJUnit4ClassRunner;
import com.android.tradefed.testtype.junit4.BaseHostJUnit4Test;
import com.android.tradefed.util.CommandResult;
import com.android.tradefed.util.ZipUtil2;

import org.apache.commons.compress.archivers.zip.ZipFile;

import org.junit.After;
import org.junit.Assert;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.File;
import java.io.IOException;
import java.lang.Process;
import java.lang.Runtime;
import java.util.concurrent.TimeUnit;

/**
 * Test Dynamic System Updates by booting in and out of a supplied system image
 */
@RunWith(DeviceJUnit4ClassRunner.class)
public class DSUEndtoEndTest extends BaseHostJUnit4Test {
    private static final long kDefaultUserdataSize = 4L * 1024 * 1024 * 1024;
    private static final String APK = "LockScreenAutomation.apk";
    private static final String PACKAGE = "com.google.android.lockscreenautomation";
    private static final String UI_AUTOMATOR_INSTRUMENTATION_RUNNER =
        "androidx.test.uiautomator.UiAutomatorInstrumentationTestRunner";
    private static final String CLASS = "LockScreenAutomation";
    private static final String SIMG2IMG_PATH = "bin/simg2img";

    // Example: atest -v DSUEndtoEndTest -- --test-arg \
    // com.android.tradefed.testtype.HostTest:set-option:system_image_path:/full/path/to/system.img
    @Option(name="system_image_path",
            shortName='s',
            description="full path to the system image to use. If not specified, attempt " +
                        "to download the image from the test infrastructure",
            importance=Importance.ALWAYS)
    private String mSystemImagePath;

    private File mUnsparseSystemImage;

    @After
    public void teardown() throws Exception {
        uninstallPackage(PACKAGE);
        if (mUnsparseSystemImage != null) {
            mUnsparseSystemImage.delete();
        }
    }

    @Test
    public void testDSU() throws Exception {
        String simg2imgPath = "simg2img";
        if (mSystemImagePath == null) {
            IDeviceBuildInfo deviceBuild = (IDeviceBuildInfo) getBuild();
            File system = deviceBuild.getDeviceImageFile();
            Assert.assertNotEquals("Failed to fetch system image. See system_image_path parameter", null, system);
            mSystemImagePath = system.getAbsolutePath();
            File otaPackage = deviceBuild.getOtaPackageFile();
            File simg2img = ZipUtil2.extractFileFromZip(new ZipFile(otaPackage), SIMG2IMG_PATH);
            simg2imgPath = simg2img.getAbsolutePath();
        }
        File gsi = new File(mSystemImagePath);
        Assert.assertTrue("not a valid file", gsi.isFile());
        String[] cmd = {simg2imgPath, mSystemImagePath, mSystemImagePath + ".raw"};
        Process p = Runtime.getRuntime().exec(cmd);
        p.waitFor();
        if (p.exitValue() == 0) {
            mUnsparseSystemImage = new File(mSystemImagePath + ".raw");
            gsi = mUnsparseSystemImage;
        }

        boolean wasRoot = getDevice().isAdbRoot();
        if (!wasRoot)
            Assert.assertTrue("Test requires root", getDevice().enableAdbRoot());

        expectGsiStatus("normal");

        installPackage(APK);
        String method = "setPin";
        String testClass = PACKAGE + "." + CLASS;
        String testMethod = testClass + "." + method;
        Assert.assertTrue(testMethod + " failed.",
            runDeviceTests(UI_AUTOMATOR_INSTRUMENTATION_RUNNER, PACKAGE, testClass, method));

        // Sleep after installing to allow time for gsi_tool to reboot. This prevents a race between
        // the device rebooting and waitForDeviceAvailable() returning.
        getDevice().executeShellV2Command("gsi_tool install --userdata-size " + kDefaultUserdataSize +
            " --gsi-size " + gsi.length() + " && sleep 10000000", gsi, null, 10, TimeUnit.MINUTES, 1);
        getDevice().waitForDeviceAvailable();
        getDevice().enableAdbRoot();

        expectGsiStatus("running");

        rebootAndUnlock();

        expectGsiStatus("installed");

        CommandResult result = getDevice().executeShellV2Command("gsi_tool enable");
        Assert.assertEquals("gsi_tool enable failed", 0, result.getExitCode().longValue());

        getDevice().reboot();

        expectGsiStatus("running");

        getDevice().reboot();

        expectGsiStatus("running");

        getDevice().executeShellV2Command("gsi_tool wipe");

        rebootAndUnlock();

        expectGsiStatus("normal");

        method = "removePin";
        testClass = PACKAGE + "." + CLASS;
        testMethod = testClass + "." + method;
        Assert.assertTrue(testMethod + " failed.",
            runDeviceTests(UI_AUTOMATOR_INSTRUMENTATION_RUNNER, PACKAGE, testClass, method));

        if (wasRoot) {
            getDevice().enableAdbRoot();
        }
    }

    private void expectGsiStatus(String expected) throws Exception {
        CommandResult result = getDevice().executeShellV2Command("gsi_tool status");
        String status = result.getStdout().split("\n", 2)[0].trim();
        Assert.assertEquals("Device not in expected DSU state", expected, status);
    }

    private void rebootAndUnlock() throws Exception {
        getDevice().rebootUntilOnline();
        getDevice().executeShellV2Command("input keyevent 224"); // KeyEvent.KEYCODE_WAKEUP
        getDevice().executeShellV2Command("wm dismiss-keyguard");
        getDevice().executeShellV2Command("input keyevent 7"); // KeyEvent.KEYCODE_0
        getDevice().executeShellV2Command("input keyevent 7");
        getDevice().executeShellV2Command("input keyevent 7");
        getDevice().executeShellV2Command("input keyevent 7");
        getDevice().executeShellV2Command("input keyevent 66"); // KeyEvent.KEYCODE_ENTER
    }
}

