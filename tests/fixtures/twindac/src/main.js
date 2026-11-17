/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A program for device/dac in the digital twin of an original ESP32
// (tests/dac.test.js): two 8-bit DACs on GPIO25 / GPIO26, 3.3 V full scale.
// It prints what each call gives, one line each, then keeps both outputs
// 1.5 s for the test to read the pins, closes the one of GPIO25, and ends
// 1.5 s later (the other stops with the program).
import { Dac } from "device/dac";

// What a call throws: the code of a system error, else the kind of error.
function outcome(run) {
    try {
        run();
        return "ok";
    } catch (error) {
        return error.code ?? error.name;
    }
}

// --- an output, its first code, what the hardware has
const out = new Dac(25, { value: 64 });
console.log(`opened: ${out.bits} ${out.fullScale} ${out.value}`);
out.write(200);
console.log(`write: ${out.value}`);

// --- codes: integers 0 to 255 only
console.log(`write 257: ${outcome(() => out.write(257))}`);
console.log(`write 256: ${outcome(() => out.write(256))}`);
console.log(`write 1.5: ${outcome(() => out.write(1.5))}`);
console.log(`write -1: ${outcome(() => out.write(-1))}`);
console.log(`write NaN: ${outcome(() => out.write(NaN))}`);
console.log(`write text: ${outcome(() => out.write("12"))}`);
console.log(`value kept: ${out.value}`);

// --- voltages: rounded to the nearest code, 0 to 3.3 V only
out.writeVoltage(1.2);
console.log(`1.2 V: ${out.value}`);
out.writeVoltage(2);
console.log(`2 V: ${out.value}`);
out.writeVoltage(3.3);
console.log(`3.3 V: ${out.value}`);
out.writeVoltage(0);
console.log(`0 V: ${out.value}`);
console.log(`voltage above: ${outcome(() => out.writeVoltage(3.31))}`);
console.log(`voltage negative: ${outcome(() => out.writeVoltage(-0.1))}`);
console.log(`voltage NaN: ${outcome(() => out.writeVoltage(NaN))}`);
console.log(`voltage text: ${outcome(() => out.writeVoltage("1"))}`);

// --- refused
console.log(`no dac: ${outcome(() => new Dac(4))}`);
console.log(`taken: ${outcome(() => new Dac(25))}`);
console.log(`pin fraction: ${outcome(() => new Dac(25.5))}`);
console.log(`pin text: ${outcome(() => new Dac("25"))}`);
console.log(`bad value: ${outcome(() => new Dac(26, { value: 256 }))}`);
console.log(`unknown option: ${outcome(() => new Dac(26, { volume: 1 }))}`);

// --- closed (the refusals above left GPIO26 free)
const other = new Dac(26, { value: 10 });
other.close();
other.close();
console.log(`closed write: ${outcome(() => other.write(1))}`);
console.log(`closed voltage: ${outcome(() => other.writeVoltage(1))}`);
console.log(`closed value: ${other.value}`);

// --- the outputs read by the test: GPIO25 at code 128, GPIO26 at 1.2 V
out.write(128);
const second = new Dac(26);
second.writeVoltage(1.2);
console.log(`second: ${second.value}`);
console.log("dac ready");

// --- one closed, then the end (the other stops with the program)
setTimeout(() => {
    out.close();
    console.log("dac closed");
}, 1500);
setTimeout(() => {
    console.log("dac end");
}, 3000);
