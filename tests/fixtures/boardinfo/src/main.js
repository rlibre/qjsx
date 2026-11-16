/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// device/board (tests/board.test.js): prints the id and the MAC addresses,
// its memory and its load after 200 ms of work and 300 ms of wait, then
// restarts once; a file of its data folder says it already did.
import { id, mac, restart, memory, load } from "device/board";
import * as fs from "sys/fs";

// the id and the addresses
console.log(`id: ${id().toHex()}`);
for (const kind of ["base", "wifi", "ap", "bluetooth", "ethernet"]) {
    console.log(`mac ${kind}: ${mac(kind).toHex()}`);
}
try {
    mac("radio");
    console.log("mac radio: accepted");
} catch (error) {
    console.log(`mac radio: ${error.name}`);
}

// the memory, then the load after 200 ms of work and 300 ms of wait
console.log(`memory: ${JSON.stringify(memory())}`);
const workEnd = Date.now() + 200;
while (Date.now() < workEnd) {
    // work
}
await new Promise(resolve => setTimeout(resolve, 300));
console.log(`load: ${JSON.stringify(load())}`);

// restart once: nothing after restart() may run, finally included
if (fs.existsSync("restarted")) {
    fs.unlinkSync("restarted");
    console.log("restarted");
} else {
    fs.writeFileSync("restarted", "1");
    console.log("restarting");
    try {
        restart();
        console.log("after restart");
    } finally {
        console.log("finally ran");
    }
}
