/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The program profiled by tests/profile.test.js: every 20 ms, about 10 ms of
// work in spin() (called by step()), the rest waiting. Runs for ever.
function spin(ms) {
    const end = Date.now() + ms;
    let turns = 0;
    while (Date.now() < end) {
        turns++;
    }
    return turns;
}

const worker = {
    step() {
        return spin(10);
    },
};

console.log("profiled: running");
setInterval(() => worker.step(), 20);
