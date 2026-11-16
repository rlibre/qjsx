/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Fixture of tests/modconst.test.js: a module whose constants are replaced
// by their value, some exported (they keep their variable), some not.
export const LIMIT = 40;
export const NAME = "board";
const STEP = 4;
const DOUBLE = STEP * 2;
export const TOTAL = LIMIT + DOUBLE;

export function steps() {
    return [STEP, DOUBLE, TOTAL, NAME];
}

export let counter = 0;

export function bump() {
    counter += STEP;
    return counter;
}
