/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// device/i2c in the digital twin ("qjsx agent --board yd-esp32-s3"), where
// nothing answers on the buses. The program tests/fixtures/twini2c prints
// what each call gives; this test checks its lines.
//
//   qjsx run tests/i2c.test.js tcp:127.0.0.1:<agent port>
import * as fs from "sys/fs";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND } from "qjsx:protocol";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const PROJECT = "tests/fixtures/twini2c";
const WAIT_TIMEOUT = 8000;

// --- the program's output, line by line
const lines = [];

function onLog(stream, text) {
    for (const line of text.split("\n").filter(item => item !== "")) {
        lines.push(line);
    }
}

// Resolves true once the program has printed 'line', false after WAIT_TIMEOUT.
async function printed(line) {
    const end = Date.now() + WAIT_TIMEOUT;
    while (!lines.includes(line) && Date.now() < end) {
        await new Promise(resolve => setTimeout(resolve, 20));
    }
    return lines.includes(line);
}

// The text the program printed after "<label>: ".
function answer(label) {
    const line = lines.find(item => item.startsWith(`${label}: `));
    return line === undefined ? undefined : line.slice(label.length + 2);
}

// --- the agent: the twin offers device/i2c
const device = await Device.open(TARGET, { onLog });
await device.hello();
check(device.natives.includes("device/i2c"), "the twin offers device/i2c");

// --- the program, to its end
const projectBuild = build(PROJECT);
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(projectBuild.file));
await device.request(KIND.RUN);
check(await printed("i2c done"), "the program ran to its end");

// --- opening a bus
equal(answer("no pins"), "TypeError", "sda and scl are required");
equal(answer("no such bus"), "RangeError", "a bus the chip does not have is refused");
equal(answer("bus taken"), "Error", "a bus already open is refused");

// --- nobody there
equal(answer("probeSync"), "false", "probeSync: nobody at the address");
equal(answer("probe"), "false", "probe: a promise of false, not an error");
equal(answer("writeSync"), "ENXIO", "writeSync throws ENXIO");
equal(answer("readSync"), "ENXIO", "readSync throws ENXIO");
equal(answer("writeReadSync"), "ENXIO", "writeReadSync throws ENXIO");
equal(answer("write"), "ENXIO", "write is rejected with ENXIO");
equal(answer("read"), "ENXIO", "read is rejected with ENXIO");
equal(answer("writeRead"), "ENXIO", "writeRead is rejected with ENXIO");

// --- arguments refused
equal(answer("byte 256"), "TypeError", "a byte above 255 is refused");
equal(answer("no bytes"), "TypeError", "an empty write is refused");
equal(answer("not bytes"), "TypeError", "a string is not bytes");
equal(answer("length 0"), "RangeError", "a read of 0 bytes is refused");
equal(answer("address 128"), "RangeError", "an address above 127 is refused");

// --- order, limits, closing
equal(answer("sync after async"), "ENXIO", "a Sync call after a queued one runs once it is done");
equal(answer("async before sync"), "ENXIO", "... and the queued one settles");
equal(answer("17th pending"), "i2c: too many transfers pending (16 at most)", "16 promises pending at most");
equal(answer("device closed"), "Error", "a closed device is refused");
equal(answer("pending at close"), "i2c: bus closed", "closing the bus rejects its pending promises");
equal(answer("after close"), "i2c: bus closed", "the devices of a closed bus are refused");
equal(answer("open again"), "false", "a closed bus can be opened again");

device.close();
report("i2c");
