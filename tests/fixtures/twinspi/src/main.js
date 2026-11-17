/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A program for device/spi in the digital twin (tests/spi.test.js), where no
// part sits on the buses: every byte read is 0xFF (MISO pulled up). It prints
// what each call gives, one line each; the test checks the lines, and the
// trace of the transactions the page gets (the bytes 0xA1 to 0xA4 mark the
// order of the writes).
import { Spi } from "device/spi";

// What a call throws: the code of a system error, else the kind of error.
function outcome(run) {
    try {
        run();
        return "ok";
    } catch (error) {
        return error.code ?? error.name;
    }
}

// What a method that returns a promise does: "ok", "rejected <code or
// kind>", or "thrown <code or kind>" (it must never throw).
async function attempt(start) {
    let promise;
    try {
        promise = start();
    } catch (error) {
        return `thrown ${error.code ?? error.name}`;
    }

    try {
        await promise;
        return "ok";
    } catch (error) {
        return `rejected ${error.code ?? error.name}`;
    }
}

// Bytes in hexadecimal: "FF FF".
function hex(bytes) {
    return Array.from(bytes, byte => byte.toString(16).toUpperCase().padStart(2, "0")).join(" ");
}

// --- opening a bus: the controllers of the flash, one the chip does not
// have, pins refused
console.log(`port 0: ${outcome(() => new Spi(0, { sclk: 12, mosi: 11 }))}`);
console.log(`port 1: ${outcome(() => new Spi(1, { sclk: 12, mosi: 11 }))}`);
console.log(`port 9: ${outcome(() => new Spi(9, { sclk: 12, mosi: 11 }))}`);
console.log(`no sclk: ${outcome(() => new Spi(2, { mosi: 11 }))}`);
console.log(`pin 1.5: ${outcome(() => new Spi(2, { sclk: 1.5, mosi: 11 }))}`);
console.log(`pin 99: ${outcome(() => new Spi(2, { sclk: 99, mosi: 11 }))}`);
console.log(`same pins: ${outcome(() => new Spi(2, { sclk: 12, mosi: 12 }))}`);

const bus = new Spi(2, { sclk: 12, mosi: 11, miso: 13 });
console.log(`bus taken: ${outcome(() => new Spi(2, { sclk: 4, mosi: 5 }))}`);
console.log(`port 3: ${outcome(() => new Spi(3, { sclk: 4, mosi: 5 }).close())}`);

// --- devices: the clock the hardware makes (80 MHz / 3 for 30 MHz)
const display = bus.device({ cs: 10, dc: 14, frequency: 30000000, mode: 3 });
console.log(`frequency: ${display.frequency}`);
const plain = bus.device({});
console.log(`default frequency: ${plain.frequency}`);
console.log(`cs on a bus pin: ${outcome(() => bus.device({ cs: 12 }))}`);
console.log(`mode 4: ${outcome(() => bus.device({ mode: 4 }))}`);
console.log(`frequency 1.5: ${outcome(() => bus.device({ frequency: 1000000.5 }))}`);

// --- a transaction with every kind of segment: nobody answers
const result = display.transactionSync([
    { write: [0x2a], dc: 0 },
    { write: [0x00, 0x00, 0x00, 0xef] },
    { delay: 10 },
    { read: 2 },
    { transfer: [1, 2, 3] },
]);
console.log(`transaction: ${result.length} | ${result.map(hex).join(" | ")}`);

// --- the shortcuts, Sync
console.log(`writeSync: ${outcome(() => display.writeSync([1, 2]))}`);
console.log(`commandSync: ${outcome(() => display.commandSync([0x2c]))}`);
console.log(`readSync: ${hex(display.readSync(4))}`);
console.log(`transferSync: ${hex(display.transferSync([1, 2]))}`);
console.log(`writeReadSync: ${hex(display.writeReadSync([0x0b], 2))}`);

// --- the shortcuts, promises
console.log(`write: ${await attempt(() => display.write([1]))}`);
console.log(`command: ${await attempt(() => display.command([1]))}`);
console.log(`read: ${hex(await display.read(2))}`);
console.log(`transfer: ${hex(await display.transfer([1]))}`);
console.log(`writeRead: ${hex(await display.writeRead([1], 3))}`);
console.log(`transaction promise: ${(await display.transaction([{ read: 1 }, { transfer: [5] }])).map(hex).join(" | ")}`);

// --- a long write: the trace keeps its first bytes and its size
display.writeSync(new Uint8Array(100).fill(0x55));

// --- Sync and promises interleaved on two devices of the bus: they run in
// the order of the calls (0xA1, 0xA2, 0xA3, 0xA4)
const queued = [plain.write([0xa1]), display.write([0xa2])];
plain.writeSync([0xa3]);
queued.push(display.write([0xa4]));
await Promise.all(queued);
console.log("order: done");

// --- arguments refused: a Sync call throws, a promise is rejected
console.log(`dc without pin: ${outcome(() => plain.transactionSync([{ write: [1], dc: 0 }]))}`);
console.log(`dc without pin, promise: ${await attempt(() => plain.transaction([{ write: [1], dc: 0 }]))}`);
console.log(`command without dc: ${outcome(() => plain.commandSync([1]))}`);
console.log(`delay 1001: ${outcome(() => display.transactionSync([{ delay: 1001 }]))}`);
console.log(`delay 1001, promise: ${await attempt(() => display.transaction([{ delay: 1001 }]))}`);
console.log(`read 1.5: ${outcome(() => display.readSync(1.5))}`);
console.log(`read 1.5, promise: ${await attempt(() => display.read(1.5))}`);
console.log(`two kinds: ${outcome(() => display.transactionSync([{ write: [1], read: 1 }]))}`);
console.log(`byte 256: ${outcome(() => display.writeSync([256]))}`);

// --- too many promises at once: 16 pending at most
const many = [];

for (let i = 0; i < 17; i++) {
    many.push(attempt(() => display.read(1)));
}

const settled = await Promise.all(many);
console.log(`17th pending: ${settled[16]}`);

// --- closing a device with transactions pending: the last ones are
// cancelled, then the device is refused
const pendingAtClose = [1, 2, 3].map(() => display.read(1).then(() => "ok", error => error.code));
display.close();
console.log(`pending at device close: ${(await Promise.all(pendingAtClose))[2]}`);
console.log(`closed device: ${outcome(() => display.readSync(1))}`);
console.log(`closed device, promise: ${await attempt(() => display.read(1))}`);

// --- closing the bus: the same for the devices left on it
const pendingAtBusClose = [1, 2, 3].map(value => plain.write([value]).then(() => "ok", error => error.code));
bus.close();
console.log(`pending at bus close: ${(await Promise.all(pendingAtBusClose))[2]}`);
console.log(`device of a closed bus: ${outcome(() => plain.writeSync([1]))}`);
console.log(`closed bus: ${outcome(() => bus.device({}))}`);

// --- the controller can be opened again
const again = new Spi(2, { sclk: 12, mosi: 11, miso: 13 });
console.log(`open again: ${hex(again.device({ cs: 10 }).readSync(1))}`);
again.close();

console.log("spi done");
