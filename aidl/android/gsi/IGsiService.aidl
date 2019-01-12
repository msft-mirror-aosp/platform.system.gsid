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

import android.os.ParcelFileDescriptor;

/** {@hide} */
interface IGsiService {
    /**
     * Begins a GSI installation.
     *
     * @param gsiSize       The size of the on-disk GSI image.
     * @param userdataSize  The desired size of the userdata partition.
     * @return              true on success, false otherwise.
     */
    boolean startGsiInstall(long gsiSize, long userdataSize);

    /**
     * Write bytes from a stream to the on-disk GSI.
     *
     * @param stream        Stream descriptor.
     * @param bytes         Number of bytes that can be read from stream.
     * @return              true on success, false otherwise.
     */
    boolean commitGsiChunkFromStream(in ParcelFileDescriptor stream, long bytes);

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
     * Remove a GSI install. This will completely remove and reclaim space used
     * by the GSI and its userdata. If currently running a GSI, space will be
     * reclaimed on the reboot.
     *
     * @return              true on success, false otherwise.
     */
    boolean removeGsiInstall();

    /**
     * Returns true if the gsi is currently running, false otherwise.
     */
    boolean isGsiRunning();
}
