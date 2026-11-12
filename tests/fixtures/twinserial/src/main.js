/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A program for device/serial in the digital twin (tests/serial.test.js):
// prints what the wrong calls give, one line each, then answers on UART 1
// what it receives, prefixed with "echo:". "close" closes the UART and opens
// it again at once; "quit" ends the program.
import { list, Serial } from "device/serial";

// What a call throws: the code of a system error, else the kind of error.
function outcome(run) {
    try {
        run();
        return "ok";
    } catch (error) {
        return error.code ?? error.name;
    }
}

// --- what the board has, what is refused
console.log(`list: ${list().join(",")}`);
console.log(`link uart: ${outcome(() => new Serial(0, { tx: 43, rx: 44 }))}`);
console.log(`no such uart: ${outcome(() => new Serial(5, { tx: 17 }))}`);
console.log(`named port: ${outcome(() => new Serial("COM3", { tx: 17 }))}`);
console.log(`no pins: ${outcome(() => new Serial(1, {}))}`);
console.log(`rts alone: ${outcome(() => new Serial(1, { tx: 17, rts: 19 }))}`);
console.log(`flowControl: ${outcome(() => new Serial(1, { tx: 17, flowControl: "rts-cts" }))}`);
console.log(`dataBits 9: ${outcome(() => new Serial(1, { tx: 17, dataBits: 9 }))}`);

// --- UART 1, answering what it receives
let uart = null;

// Opens UART 1 and answers on it.
function open() {
    uart = new Serial(1, { baudRate: 9600, parity: "even", tx: 17, rx: 18 });
    uart.onData(bytes => {
        const text = new TextDecoder().decode(bytes);
        if (text === "close") {
            uart.close();
            return;
        }
        if (text === "quit") {
            uart.onClose(() => console.log("serial done"));
            uart.close();
            return;
        }
        uart.write(`echo:${text}`);
    });
    uart.onClose(() => {
        console.log("closed");
        open();
        console.log(`reopened: ${uart.isOpen()}`);
    });
}

open();
console.log(`port: ${uart.port()}`);
console.log(`taken: ${outcome(() => new Serial(1, { tx: 17 }))}`);
console.log(`list open: ${list().join(",")}`);
console.log("serial ready");
