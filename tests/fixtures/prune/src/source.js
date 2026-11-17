/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Fixture of tests/prune.test.js: 'dropped' is re-exported, never imported
// by name, but imported by a module with eval (kept); 'forgotten' is unused.
export function reexported() {
    return "re";
}

export function dropped() {
    return "dropped";
}

export function forgotten() {
    return "forgotten";
}
