/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A program for device/i2c in the digital twin (tests/i2c.test.js), where
// nothing answers on the buses: every transfer ends with ENXIO. It prints
// what each call gives, one line each; the test checks the lines.
import { I2c } from "device/i2c";

// What a call throws: the code of a system error, else the kind of error.
function outcome(run) {
    try {
        run();
        return "ok";
    } catch (error) {
        return error.code ?? error.name;
    }
}

// What a promise is rejected with (or "ok").
async function settled(promise) {
    try {
        await promise;
        return "ok";
    } catch (error) {
        return error.code ?? error.name;
    }
}

// --- opening a bus
console.log(`no pins: ${outcome(() => new I2c(0, {}))}`);
console.log(`no such bus: ${outcome(() => new I2c(5, { sda: 1, scl: 2 }))}`);
const bus = new I2c(0, { sda: 8, scl: 9, frequency: 400000 });
console.log(`bus taken: ${outcome(() => new I2c(0, { sda: 10, scl: 11 }))}`);

// --- nobody there
console.log(`probeSync: ${bus.probeSync(0x70)}`);
console.log(`probe: ${await bus.probe(0x70)}`);

const device = bus.device(0x48, { frequency: 100000, timeout: 50 });
console.log(`writeSync: ${outcome(() => device.writeSync([1, 2]))}`);
console.log(`readSync: ${outcome(() => device.readSync(2))}`);
console.log(`writeReadSync: ${outcome(() => device.writeReadSync(Uint8Array.of(0), 2))}`);
console.log(`write: ${await settled(device.write([1]))}`);
console.log(`read: ${await settled(device.read(2))}`);
console.log(`writeRead: ${await settled(device.writeRead([0], 2))}`);

// --- arguments refused
console.log(`byte 256: ${outcome(() => device.writeSync([256]))}`);
console.log(`no bytes: ${outcome(() => device.writeSync([]))}`);
console.log(`not bytes: ${outcome(() => device.writeSync("text"))}`);
console.log(`length 0: ${outcome(() => device.readSync(0))}`);
console.log(`address 128: ${outcome(() => bus.device(128))}`);

// --- a Sync call after queued ones waits for them, without blocking for good
const queued = settled(device.write([1]));
console.log(`sync after async: ${outcome(() => device.writeSync([2]))}`);
console.log(`async before sync: ${await queued}`);

// --- too many promises at once
const many = [];
let refused = "none";
for (let i = 0; i < 17; i++) {
    try {
        many.push(settled(device.read(1)));
    } catch (error) {
        refused = error.message;
    }
}
console.log(`17th pending: ${refused}`);
await Promise.all(many);

// --- a device closed, then the bus closed with a transfer pending
const second = bus.device(0x49);
second.close();
console.log(`device closed: ${outcome(() => second.readSync(1))}`);

const late = device.read(1).then(() => "ok", error => error.message);
bus.close();
console.log(`pending at close: ${await late}`);
let afterClose = "";
try {
    device.readSync(1);
} catch (error) {
    afterClose = error.message;
}
console.log(`after close: ${afterClose}`);

// --- the bus can be taken again
const again = new I2c(0, { sda: 8, scl: 9 });
console.log(`open again: ${again.probeSync(0x70)}`);
again.close();

console.log("i2c done");
