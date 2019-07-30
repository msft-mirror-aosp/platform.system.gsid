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

import android.gsi.IGsiService;

/** {@hide} */
interface IGsid {
    // Acquire an IGsiService client. gsid automatically shuts down when the
    // last client is dropped. To start the daemon:
    //
    //  1. Check if the "init.svc.gsid" property is "running". If not, continue.
    //  2. Set the "ctl.start" property to "gsid".
    //  3. Wait for "init.svc.gsid" to be "running".
    IGsiService getClient();
}
