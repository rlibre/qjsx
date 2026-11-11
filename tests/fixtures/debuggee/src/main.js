/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Program debugged by tests/debugger.test.js. The test finds its stops by
// the text of the statements below: change both together.
import { area } from "./shapes.js";

let keepSpinning = true;

function compute(width, height) {
    const box = { width, height, label: "box" };
    const result = area(box);
    return result;
}

// Runs until the debugger sets 'keepSpinning' to false.
function spin() {
    let turns = 0;
    while (keepSpinning) {
        turns++;
    }
    return turns;
}

function failSoftly() {
    try {
        throw new Error("expected failure");
    } catch (error) {
        return error.message;
    }
}

// Step over the end of first(): second() runs next, at the same place of
// the C stack; the step must not stop in it.
function first() {
    debugger;
    return 1;
}

function second() {
    return 2;
}

function add(x, y) {
    return x + y;
}

// Step over the 'await' while the timer of tick() fires: the step must go
// on in waitAndCount(), not stop in tick(). Then step out of waitAndCount(),
// resumed by a promise job: the next code run is showTicks().
let ticks = 0;

function tick() {
    ticks++;
}

function sleep(milliseconds) {
    return new Promise(resolve => setTimeout(resolve, milliseconds));
}

async function waitAndCount() {
    setTimeout(tick, 0);
    debugger;
    await sleep(20);
    return ticks;
}

function showTicks(count) {
    console.log("ticks", count);
    console.log("done");
}

console.log("area", compute(3, 4));
console.log("spun", spin() > 0);
console.log(failSoftly());
debugger;
console.log("sum", add(first(), second()));
waitAndCount().then(showTicks);
