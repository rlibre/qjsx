/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Measures of the engine on the PC, run by tools/profile/profile.sh through
// qjsx-tests (module "profile_test", tests/native/profile_module.cpp):
//   1. the cost of each kind of call, in ns per call (empty loop removed);
//   2. a profile of each test of C:\temp\qjsx-speed (the program measured
//      on the boards), scaled up to run about a second on the PC: the
//      sampler's lines ("sample <address> <count>") follow a line
//      "profile <name>".
import { start, stop, nop, rawNop, Bench } from "profile_test";

const CALLS = 5_000_000;   // iterations of each call measure

// ### 1. calls ################################################################

/**
 * Nanoseconds per turn of 'run' (a loop of CALLS turns), best of 3.
 */
function nsPerTurn(run) {
    let fastest = Infinity;
    for (let round = 0; round < 3; round++) {
        const begin = Date.now();
        run();
        fastest = Math.min(fastest, Date.now() - begin);
    }

    return (fastest * 1e6) / CALLS;
}

function empty() {
}

const bench = new Bench();
const point = { x: 1, y: 2 };

const emptyLoop = nsPerTurn(() => {
    for (let i = 0; i < CALLS; i++) {
    }
});

const measures = [
    ["JS function f()", () => { for (let i = 0; i < CALLS; i++) { empty(); } }],
    ["plain C function rawNop()", () => { for (let i = 0; i < CALLS; i++) { rawNop(); } }],
    ["jsbind function nop()", () => { for (let i = 0; i < CALLS; i++) { nop(); } }],
    ["jsbind method bench.nop()", () => { for (let i = 0; i < CALLS; i++) { bench.nop(); } }],
    ["jsbind method bench.add(1, 2)", () => { for (let i = 0; i < CALLS; i++) { bench.add(1, 2); } }],
    ["property read point.x", () => { let s = 0; for (let i = 0; i < CALLS; i++) { s += point.x; } return s; }],
];

console.log(`calls: empty loop ${emptyLoop.toFixed(1)} ns per turn; cost of each call, loop removed:`);
for (const [name, run] of measures) {
    const cost = nsPerTurn(run) - emptyLoop;
    console.log(`  ${name.padEnd(32)} ${cost.toFixed(1).padStart(6)} ns`);
}

// ### 2. profiles of the speed tests ##########################################

const SAMPLES_PER_SECOND = 10000;

/**
 * Runs 'work' under the sampler, after a line "profile <name>".
 */
function profile(name, work) {
    console.log(`profile ${name}`);
    start(SAMPLES_PER_SECOND);
    work();
    stop();
}

profile("loop (int arithmetic)", () => {
    let sum = 0;
    for (let i = 0; i < 40_000_000; i++) {
        sum = (sum + i * 7) % 1000003;
    }

    return sum;
});

function add(a, b) {
    return a + b;
}

profile("function calls", () => {
    let sum = 0;
    for (let i = 0; i < 20_000_000; i++) {
        sum = add(sum, i) & 0xffff;
    }

    return sum;
});

profile("property reads", () => {
    const p = { x: 1, y: 2, z: 3 };
    let sum = 0;
    for (let i = 0; i < 40_000_000; i++) {
        sum += p.x + p.y;
    }

    return sum;
});

profile("small objects", () => {
    let kept = null;
    for (let i = 0; i < 4_000_000; i++) {
        kept = { index: i, next: kept && i % 100 !== 0 ? kept : null };
    }

    return kept;
});

profile("string concatenations", () => {
    let text = "";
    for (let i = 0; i < 2_000_000; i++) {
        text += String.fromCharCode(65 + (i % 26));
        if (text.length > 1000) {
            text = "";
        }
    }

    return text;
});

profile("Uint8Array writes", () => {
    const bytes = new Uint8Array(4096);
    for (let i = 0; i < 20_000_000; i++) {
        bytes[i & 4095] = i;
    }

    return bytes[0];
});

profile("Math.sin", () => {
    let sum = 0;
    for (let i = 0; i < 10_000_000; i++) {
        sum += Math.sin(i * 0.001);
    }

    return sum;
});

profile("method calls bench.nop()", () => {
    for (let i = 0; i < 20_000_000; i++) {
        bench.nop();
    }
});
