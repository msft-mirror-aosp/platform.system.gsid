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

package android.gsi;

import android.gsi.GsiProgress;
import android.os.ParcelFileDescriptor;

/** {@hide} */
interface IGsiService {
    /* Status codes for GsiProgress.status */
    const int STATUS_NO_OPERATION = 0;
    const int STATUS_WORKING = 1;
    const int STATUS_COMPLETE = 2;

    /**
     * Begins a GSI installation.
     *
     * If wipeUserData is true, a clean userdata image is always created to the
     * desired size.
     *
     * If wipeUserData is false, a userdata image is only created if one does
     * not already exist. If the size is zero, a default size of 8GiB is used.
     * If there is an existing image smaller than the desired size, it is
     * resized automatically.
     *
     * @param gsiSize       The size of the on-disk GSI image.
     * @param userdataSize  The desired size of the userdata partition.
     * @param wipeUserdata  True to wipe destination userdata.
     * @return              true on success, false otherwise.
     */
    boolean startGsiInstall(long gsiSize, long userdataSize, boolean wipeUserdata);

    /**
     * Write bytes from a stream to the on-disk GSI.
     *
     * @param stream        Stream descriptor.
     * @param bytes         Number of bytes that can be read from stream.
     * @return              true on success, false otherwise.
     */
    boolean commitGsiChunkFromStream(in ParcelFileDescriptor stream, long bytes);

    /**
     * Query the progress of the current asynchronous install operation. This
     * can be called while another operation is in progress.
     */
    GsiProgress getInstallProgress();

    /**
     * Write bytes from memory to the on-disk GSI.
     *
     * @param bytes         Byte array.
     * @return              true on success, false otherwise.
     */
    boolean commitGsiChunkFromMemory(in byte[] bytes);

    /**
     * Complete a GSI installation and mark it as bootable. The caller is
     * responsible for rebooting the device as soon as possible.
     *
     * @return              true on success, false otherwise.
     */
    boolean setGsiBootable();

    /**
     * Cancel an in-progress GSI install.
     */
    boolean cancelGsiInstall();

    /**
     * Return if a GSI installation is currently in-progress.
     */
    boolean isGsiInstallInProgress();

    /**
     * Remove a GSI install. This will completely remove and reclaim space used
     * by the GSI and its userdata. If currently running a GSI, space will be
     * reclaimed on the reboot.
     *
     * @return              true on success, false otherwise.
     */
    boolean removeGsiInstall();

    /**
     * Disables a GSI install. The image and userdata will be retained, but can
     * be re-enabled at any time with setGsiBootable.
     */
    boolean disableGsiInstall();

    /**
     * Return the size of the userdata partition for an installed GSI. If there
     * is no image, 0 is returned. On error, -1 is returned.
     */
    long getUserdataImageSize();

    /**
     * Returns true if the gsi is currently running, false otherwise.
     */
    boolean isGsiRunning();

    /**
     * Returns true if a gsi is installed.
     */
    boolean isGsiInstalled();
}
