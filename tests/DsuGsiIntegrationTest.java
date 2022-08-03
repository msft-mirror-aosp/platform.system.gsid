/*
 * Copyright (C) 2022 The Android Open Source Project
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

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import com.android.tradefed.config.Option;
import com.android.tradefed.device.DeviceNotAvailableException;
import com.android.tradefed.log.LogUtil.CLog;
import com.android.tradefed.testtype.DeviceJUnit4ClassRunner;
import com.android.tradefed.testtype.junit4.BaseHostJUnit4Test;
import com.android.tradefed.util.CommandResult;
import com.android.tradefed.util.CommandStatus;
import com.android.tradefed.util.FileUtil;
import com.android.tradefed.util.StreamUtil;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

@RunWith(DeviceJUnit4ClassRunner.class)
public class DsuGsiIntegrationTest extends BaseHostJUnit4Test {
    private static final long DSU_MAX_WAIT_SEC = 10 * 60;
    private static final long DSU_USERDATA_SIZE = 8L << 30;

    private static final String GSI_IMAGE_NAME = "system.img";
    private static final String DSU_IMAGE_ZIP_PUSH_PATH = "/sdcard/gsi.zip";
    private static final String DSU_INSTALL_COMMAND =
            String.format(
                    "am start-activity"
                            + " -n com.android.dynsystem/com.android.dynsystem.VerificationActivity"
                            + " -a android.os.image.action.START_INSTALL"
                            + " -d file://%s"
                            + " --el KEY_USERDATA_SIZE %d"
                            + " --ez KEY_ENABLE_WHEN_COMPLETED true",
                    DSU_IMAGE_ZIP_PUSH_PATH, DSU_USERDATA_SIZE);

    private static final String REMOUNT_TEST_PATH = "/system/remount_test";
    private static final String REMOUNT_TEST_FILE = REMOUNT_TEST_PATH + "/test_file";

    @Option(
            name = "wipe-dsu-on-failure",
            description = "Wipe the DSU installation on test failure.")
    private boolean mWipeDsuOnFailure = true;

    @Option(
            name = "system-image-path",
            description = "Path to the GSI system.img or directory containing the system.img.",
            mandatory = true)
    private File mSystemImagePath;

    private File mSystemImageZip;

    @Before
    public void setUp() throws IOException {
        mSystemImageZip = null;
        InputStream stream = null;
        try {
            assertNotNull("--system-image-path is invalid", mSystemImagePath);
            if (mSystemImagePath.isDirectory()) {
                File gsiImageFile = FileUtil.findFile(mSystemImagePath, GSI_IMAGE_NAME);
                assertNotNull("Cannot find " + GSI_IMAGE_NAME, gsiImageFile);
                stream = new FileInputStream(gsiImageFile);
            } else {
                stream = new FileInputStream(mSystemImagePath);
            }
            stream = new BufferedInputStream(stream);
            mSystemImageZip = FileUtil.createTempFile(this.getClass().getSimpleName(), "gsi.zip");
            try (FileOutputStream foStream = new FileOutputStream(mSystemImageZip);
                    BufferedOutputStream boStream = new BufferedOutputStream(foStream);
                    ZipOutputStream out = new ZipOutputStream(boStream); ) {
                // Don't bother compressing it as we are going to uncompress it on device anyway.
                out.setLevel(0);
                out.putNextEntry(new ZipEntry(GSI_IMAGE_NAME));
                StreamUtil.copyStreams(stream, out);
                out.closeEntry();
            }
        } finally {
            StreamUtil.close(stream);
        }
    }

    @After
    public void tearDown() {
        try {
            FileUtil.deleteFile(mSystemImageZip);
        } catch (SecurityException e) {
            CLog.w("Failed to clean up '%s': %s", mSystemImageZip, e);
        }
        if (mWipeDsuOnFailure) {
            // If test case completed successfully, then the test body should have called `wipe`
            // already and calling `wipe` again would be a noop.
            // If test case failed, then this piece of code would clean up the DSU installation left
            // by the failed test case.
            try {
                getDevice().executeShellV2Command("gsi_tool wipe");
                if (isDsuRunning()) {
                    getDevice().reboot();
                }
            } catch (DeviceNotAvailableException e) {
                CLog.w("Failed to clean up DSU installation on device: %s", e);
            }
        }
        try {
            getDevice().deleteFile(DSU_IMAGE_ZIP_PUSH_PATH);
        } catch (DeviceNotAvailableException e) {
            CLog.w("Failed to clean up device '%s': %s", DSU_IMAGE_ZIP_PUSH_PATH, e);
        }
    }

    private CommandResult assertShellCommand(String command) throws DeviceNotAvailableException {
        CommandResult result = getDevice().executeShellV2Command(command);
        assertEquals(CommandStatus.SUCCESS, result.getStatus());
        assertNotNull(result.getExitCode());
        assertEquals(0, result.getExitCode().intValue());
        return result;
    }

    private boolean isDsuRunning() throws DeviceNotAvailableException {
        CommandResult result;
        try {
            result = assertShellCommand("gsi_tool status");
        } catch (AssertionError e) {
            CLog.e(e);
            return false;
        }
        return result.getStdout().split("\n", 2)[0].trim().equals("running");
    }

    private void assertDsuRunning() throws DeviceNotAvailableException {
        assertTrue("Expected DSU running", isDsuRunning());
    }

    private void assertDsuNotRunning() throws DeviceNotAvailableException {
        assertFalse("Expected DSU not running", isDsuRunning());
    }

    private void assertAdbRoot() throws DeviceNotAvailableException {
        assertTrue("Failed to 'adb root'", getDevice().enableAdbRoot());
    }

    private void assertDevicePathExist(String path) throws DeviceNotAvailableException {
        assertTrue(String.format("Expected '%s' to exist", path), getDevice().doesFileExist(path));
    }

    private void assertDevicePathNotExist(String path) throws DeviceNotAvailableException {
        assertFalse(
                String.format("Expected '%s' to not exist", path), getDevice().doesFileExist(path));
    }

    @Test
    public void testDsuGsi() throws DeviceNotAvailableException {
        if (isDsuRunning()) {
            CLog.i("Wipe existing DSU installation");
            assertShellCommand("gsi_tool wipe");
            getDevice().reboot();
            assertDsuNotRunning();
        }

        CLog.i("Pushing '%s' -> '%s'", mSystemImageZip, DSU_IMAGE_ZIP_PUSH_PATH);
        getDevice().pushFile(mSystemImageZip, DSU_IMAGE_ZIP_PUSH_PATH);

        assertShellCommand(DSU_INSTALL_COMMAND);
        CLog.i("Wait for DSU installation complete and reboot");
        assertTrue(
                "Timed out waiting for DSU installation complete",
                getDevice().waitForDeviceNotAvailable(DSU_MAX_WAIT_SEC * 1000));
        CLog.i("DSU installation is complete and device is disconnected");

        getDevice().waitForDeviceAvailable();
        assertDsuRunning();
        CLog.i("Successfully booted with DSU");

        CLog.i("Test 'gsi_tool enable -s' and 'gsi_tool enable'");
        getDevice().reboot();
        assertDsuNotRunning();
        assertShellCommand("gsi_tool enable");
        getDevice().reboot();
        assertDsuRunning();

        CLog.i("Set up 'adb remount' for testing (requires reboot)");
        assertAdbRoot();
        assertShellCommand("remount");
        getDevice().reboot();
        assertDsuRunning();
        assertAdbRoot();
        assertShellCommand("remount");
        assertDevicePathNotExist(REMOUNT_TEST_PATH);
        assertShellCommand(String.format("mkdir -p '%s'", REMOUNT_TEST_PATH));
        assertShellCommand(String.format("cp /system/bin/init '%s'", REMOUNT_TEST_FILE));
        final String initContent = getDevice().pullFileContents("/system/bin/init");

        CLog.i("DSU and original system have separate remount overlays");
        assertShellCommand("gsi_tool disable");
        getDevice().reboot();
        assertDsuNotRunning();
        assertDevicePathNotExist(REMOUNT_TEST_PATH);

        CLog.i("Test that 'adb remount' is consistent after reboot");
        assertShellCommand("gsi_tool enable");
        getDevice().reboot();
        assertDsuRunning();
        assertDevicePathExist(REMOUNT_TEST_FILE);
        assertEquals(
                String.format(
                        "Expected contents of '%s' to persist after reboot", REMOUNT_TEST_FILE),
                initContent,
                getDevice().pullFileContents(REMOUNT_TEST_FILE));

        CLog.i("'enable-verity' should teardown the remount overlay and restore the filesystem");
        assertAdbRoot();
        assertShellCommand("enable-verity");
        getDevice().reboot();
        assertDsuRunning();
        assertDevicePathNotExist(REMOUNT_TEST_PATH);

        CLog.i("Testing is done, clean up the device");
        assertShellCommand("gsi_tool wipe");
        getDevice().reboot();
        assertDsuNotRunning();
    }
}
