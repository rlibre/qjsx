/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// process.exit inside an async function: no catch, no rejection handler, no
// timer runs afterwards, and `qjsx run` returns the code (6).
(async () => {
    await null;
    try {
        process.exit(6);
    } catch {
        console.log("catch should not run");
    }
})().catch(() => {
    console.log("rejection handler should not run");
});
setTimeout(() => {
    console.log("timer should not run");
}, 20);
