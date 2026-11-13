/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// tools/profile/workload.js under the sampler of qjsx-tests (module
// "profile_test"): one profile per part, for tools/profile/profile.sh:
//   bash tools/profile/profile.sh Profile 15 tools/profile/workload-profiled.js
import { start, stop } from "profile_test";

const SAMPLES_PER_SECOND = 10000;

// the hooks workload.js calls around each of its parts
globalThis.workloadHooks = {
    begin(name) {
        console.log(`profile ${name}`);
        start(SAMPLES_PER_SECOND);
    },
    end() {
        stop();
    },
};

await import("./workload.js");
