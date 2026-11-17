/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Fixture of tests/prune.test.js: a direct eval may use any name, so this
// module keeps everything ('kept' is never called by name) and what it
// imports ('dropped' is kept in source.js only through this import).
import { dropped } from "./source.js";

export function maybeEval(text) {
    return eval(text);
}

function kept() {
    return "kept";
}
