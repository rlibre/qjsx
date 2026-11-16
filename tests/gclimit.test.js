/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The cycle collector of a heap of a fixed size (engine change T): run with
// QJSX_JS_HEAP_MB=1, the runtime gives QuickJS the size of the region as
// its memory limit. Live data fill most of it, then cycles are made in a
// loop, far more than the heap holds: without the cap of the GC threshold
// (QuickJS raises it to 1.5 x the heap after a GC, past the region), they
// would never be collected and the loop would run out of memory.
// Run with: QJSX_JS_HEAP_MB=1 bin/Release/qjsx run tests/gclimit.test.js
import { check, report } from "./assert.js";

// Live data kept until the end, in pieces: with what the runtime itself
// holds, 1.5 x the heap is past the 1 MB region. Measured on the PC (Debug,
// 2026-11-16): with the original threshold 400 to 600 pieces run out of
// memory, with the cap they all pass; 450 is in the middle
const PIECES = 450;
const live = [];
for (let i = 0; i < PIECES; i++) {
    live.push(new Uint8Array(1000));
}

/**
 * Makes one cycle: an object and a closure that refer to each other, as a
 * callback that removes itself; nothing keeps them once it returns.
 */
function makeCycle(index) {
    const holder = { index, payload: new Array(16).fill(index) };
    holder.done = () => holder.index;
    return holder.done();
}

// ten times what the heap holds, in cycles
let made = 0;
let failure = null;
try {
    for (let i = 0; i < 60000; i++) {
        made += makeCycle(i) === i ? 1 : 0;
    }
} catch (error) {
    failure = error;
}

check(failure === null, `the cycles are collected before the heap is full (${failure})`);
check(made === 60000, `every cycle was made (${made})`);
check(live.length === PIECES, "the live data are still there");
report("gclimit");
