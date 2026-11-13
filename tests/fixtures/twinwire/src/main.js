/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A program for the wiring of the twin's UARTs to PC serial ports
// (tests/twinwire.test.js): opens UARTs 1 and 2, then waits; "quit" on
// UART 1 (sent by the page) ends it.
import { Serial } from "device/serial";

const first = new Serial(1, { baudRate: 9600, tx: 17, rx: 18 });
const second = new Serial(2, { baudRate: 115200, tx: 4, rx: 5 });
console.log("uarts open");

first.onData(bytes => {
    if (new TextDecoder().decode(bytes) === "quit") {
        first.close();
        second.close();
        console.log("wire done");
    }
});
