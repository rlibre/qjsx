/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Functions debugged by tests/debug.test.js. Its checks depend on the
// statements below: change both together.
export function add(a, b) {
    const sum = a + b;
    let doubled = sum * 2;
    return doubled;
}

export function loop(n) {
    let total = 0;
    for (let i = 0; i < n; i++) {
        total += i;
    }
    return total;
}

export function withDebugger(x) {
    debugger;
    return x + 1;
}

export function tdz() {
    debugger;
    let later = 1;
    return later;
}

export function thrower() {
    throw new Error("boom");
}

export function catcher() {
    try {
        thrower();
    } catch (e) {
        return "caught";
    }
}

export function throughFinally() {
    try {
        thrower();
    } finally {
        // nothing: the exception goes on
    }
}

export let cleanups = 0;

export function throughFinallyWithCode() {
    try {
        thrower();
    } finally {
        cleanups++;
    }
}

export async function asyncThrower() {
    await null;
    throw new Error("async boom");
}

export async function asyncCatcher() {
    try {
        await asyncThrower();
    } catch (e) {
        return "async caught";
    }
}
