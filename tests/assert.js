/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Minimal test helpers (shared by the test scripts).
let failures = 0;
let checks = 0;

export function check(condition, message) {
    checks++;
    if (!condition) {
        failures++;
        console.error("FAIL:", message);
    }
}

export function equal(actual, expected, message) {
    const same = Object.is(actual, expected);
    check(same, `${message}: expected ${String(expected)}, got ${String(actual)}`);
}

// Checks that 'run' throws an error whose text matches 'pattern'.
export function throws(run, pattern, message) {
    try {
        run();
    } catch (error) {
        const text = `${error.name}: ${error.message}`;
        check(pattern.test(text), `${message}: unexpected error "${text}"`);
        return error;
    }
    check(false, `${message}: no error thrown`);
    return undefined;
}

export async function rejects(promise, pattern, message) {
    try {
        await promise;
    } catch (error) {
        const text = `${error.name}: ${error.message}`;
        check(pattern.test(text), `${message}: unexpected rejection "${text}"`);
        return;
    }
    check(false, `${message}: not rejected`);
}

export function report(name) {
    console.log(`${name}: ${checks - failures}/${checks} checks passed`);
    if (failures > 0) {
        process.exit(1);
    }
}
