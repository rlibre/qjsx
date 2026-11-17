/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Fixture of tests/prune.test.js: imported with 'import * as', every export
// stays (even 'three', which the program does not call).
export function one() {
    return 1;
}

export function two() {
    return 2;
}

export function three() {
    return 3;
}

// not exported, not used: left out
function local() {
    return 0;
}
