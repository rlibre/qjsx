/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Writes to stderr, then throws from a timer: on a device the exception is
// printed and the program goes on (here: the next timer still runs).
console.error("to stderr");
setTimeout(() => {
    throw new Error("boom");
}, 1);
setTimeout(() => {
    console.log("still running after the exception");
}, 20);
