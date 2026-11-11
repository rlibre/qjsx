/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// process.exit ends the program, never the agent: nothing runs after it (not
// even a finally block or a pending timer), and the agent reports the code.
setTimeout(() => {
    console.log("timer should not run");
}, 20);
console.log("before exit");
try {
    process.exit(3);
} finally {
    console.log("finally should not run");
}
console.log("after exit should not run");
