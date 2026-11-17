/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Fixture of tests/prune.test.js: declarations used and unused.
export function used(x) {
    return helper(x) + 1;
}

function helper(x) {
    return x * 10;
}

// uses helper, but nothing uses it: left out
function unusedHelper() {
    return helper(0);
}

// a computed key that reads a property: still left out when unused
export class Shape {
    constructor(side) {
        this.side = side;
    }

    area() {
        return this.side ** 2;
    }

    [Symbol.iterator]() {
        return [this.side][Symbol.iterator]();
    }
}

export class UnusedShape extends Shape {}

// one statement, two names: kept whole since 'value' is used
const value = 42, other = 7;
export { value, other as renamed };

export const UNUSED_CONSTANT = "été";   // non-ASCII text left out: the offsets stay

// a side effect: kept although nothing imports it
let sideEffect = 0;
export const counted = (sideEffect++, sideEffect);

export default function () {
    return "default";
}
