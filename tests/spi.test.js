/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// device/spi in the digital twin ("qjsx agent --board yd-esp32-s3"), where
// no part sits on the buses: every byte read is 0xFF. The program
// tests/fixtures/twinspi prints what each call gives; this test checks its
// lines, then the trace of the transactions the page gets (twin.spi.data).
//
//   qjsx run tests/spi.test.js tcp:127.0.0.1:<agent port> <page port>
import * as fs from "sys/fs";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND } from "qjsx:protocol";
import { connect } from "qjsx:websocket";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const PAGE_PORT = Number(process.argv[3]);
const PROJECT = "tests/fixtures/twinspi";
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

// --- the SPI transactions the page gets, in their order
const records = [];

function onPageMessage(text) {
    const message = JSON.parse(text);
    if (message.msg === "twin.spi.data") {
        records.push(...message.params.records);
    }
}

// The records of the trace once 'test' holds for them, after WAIT_TIMEOUT
// at most.
async function recordsWhen(test) {
    const end = Date.now() + WAIT_TIMEOUT;
    while (!test(records) && Date.now() < end) {
        await new Promise(resolve => setTimeout(resolve, 20));
    }
    return records;
}

// --- the agent: the twin offers device/spi; the page, to watch the trace
const device = await Device.open(TARGET, { onLog });
await device.hello();
check(device.natives.includes("device/spi"), "the twin offers device/spi");
const socket = await connect({ port: PAGE_PORT, path: "/ws", protocol: "messaging" });
socket.onMessage(onPageMessage);

// --- the program, to its end
const projectBuild = build(PROJECT);
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(projectBuild.file));
await device.request(KIND.RUN);
check(await printed("spi done"), "the program ran to its end");

// --- opening a bus
equal(answer("port 0"), "EBUSY", "controller 0 (the flash's) is busy");
equal(answer("port 1"), "EBUSY", "controller 1 (the flash's) is busy");
equal(answer("port 9"), "ENOENT", "a controller the chip does not have is refused");
equal(answer("no sclk"), "TypeError", "sclk is required");
equal(answer("pin 1.5"), "RangeError", "a pin that is not an integer is refused, not truncated");
equal(answer("pin 99"), "EINVAL", "a pin the board does not have is refused");
equal(answer("same pins"), "EINVAL", "two lines on one pin are refused");
equal(answer("bus taken"), "EBUSY", "a controller open already is refused");
equal(answer("port 3"), "ok", "the other controller opens");

// --- devices
equal(answer("frequency"), "26666666", "the clock is what the hardware makes (80 MHz / 3 for 30 MHz)");
equal(answer("default frequency"), "1000000", "1 MHz by default");
equal(answer("cs on a bus pin"), "EINVAL", "a chip select on a pin of the bus is refused");
equal(answer("mode 4"), "RangeError", "a mode above 3 is refused");
equal(answer("frequency 1.5"), "RangeError", "a frequency that is not an integer is refused");

// --- transactions with nobody on the bus: 0xFF read
equal(answer("transaction"), "2 | FF FF | FF FF FF", "one result per read and transfer segment, 0xFF read");
equal(answer("writeSync"), "ok", "writeSync");
equal(answer("commandSync"), "ok", "commandSync");
equal(answer("readSync"), "FF FF FF FF", "readSync");
equal(answer("transferSync"), "FF FF", "transferSync");
equal(answer("writeReadSync"), "FF FF", "writeReadSync");
equal(answer("write"), "ok", "write");
equal(answer("command"), "ok", "command");
equal(answer("read"), "FF FF", "read");
equal(answer("transfer"), "FF", "transfer");
equal(answer("writeRead"), "FF FF FF", "writeRead");
equal(answer("transaction promise"), "FF | FF", "transaction");

// --- arguments refused: a Sync call throws, a promise is rejected
equal(answer("dc without pin"), "TypeError", "a DC level on a device without DC pin is refused");
equal(answer("dc without pin, promise"), "rejected TypeError", "... a promise is rejected, never thrown");
equal(answer("command without dc"), "TypeError", "command needs a DC pin");
equal(answer("delay 1001"), "RangeError", "a delay above 1000 µs is refused");
equal(answer("delay 1001, promise"), "rejected RangeError", "... a promise is rejected");
equal(answer("read 1.5"), "RangeError", "a size that is not an integer is refused");
equal(answer("read 1.5, promise"), "rejected RangeError", "... a promise is rejected");
equal(answer("two kinds"), "TypeError", "a segment of two kinds is refused");
equal(answer("byte 256"), "RangeError", "a byte above 255 is refused");

// --- limits, closing
equal(answer("17th pending"), "rejected EAGAIN", "16 promises pending at most");
equal(answer("pending at device close"), "ECANCELED", "closing a device cancels its pending transactions");
equal(answer("closed device"), "EBADF", "a closed device is refused");
equal(answer("closed device, promise"), "rejected EBADF", "... a promise is rejected");
equal(answer("pending at bus close"), "ECANCELED", "closing the bus cancels the pending transactions");
equal(answer("device of a closed bus"), "EBADF", "the devices of a closed bus are refused");
equal(answer("closed bus"), "EBADF", "a closed bus makes no device");
equal(answer("open again"), "FF", "a closed controller can be opened again");

// --- the trace of the page: the transaction of every kind
await recordsWhen(list => list.some(record => record.segments.some(segment => segment.written[0] === 0xa4)));
const full = records.find(record => record.segmentCount === 5);
equal(full?.port, 2, "the page gets the trace: the controller");
equal(full?.cs, 10, "... the chip select");
equal(full?.dc, 14, "... the DC pin");
equal(full?.frequency, 26666666, "... the clock");
equal(full?.mode, 3, "... the mode");
equal(full?.outcome, "nobody", "... nobody answered");
equal(full?.segments.map(segment => segment.kind).join(","), "write,write,delay,read,transfer", "... the kinds of the segments");
equal(`${full?.segments[0].dc},${full?.segments[1].dc}`, "0,1", "... the level of the DC pin of the writes");
equal(full?.segments[2].delayUs, 10, "... the length of the delay");
equal(JSON.stringify(full?.segments[4].written), "[1,2,3]", "... the bytes a transfer sent");
equal(JSON.stringify(full?.segments[4].read), "[255,255,255]", "... and received");

// the long write: its first 16 bytes, its size
const long = records.find(record => record.segments[0]?.size === 100);
check(long?.segments[0].written.length === 16 && long?.segments[0].written[0] === 0x55,
    "a long write is kept with its first 16 bytes and its size");

// the order of the calls: promises and a Sync call interleaved
const marks = records.map(record => record.segments[0]?.written[0]).filter(byte => byte >= 0xa1 && byte <= 0xa4);
equal(marks.map(byte => byte.toString(16)).join(","), "a1,a2,a3,a4", "the transactions run in the order of the calls");
const plainWrite = records.find(record => record.segments[0]?.written[0] === 0xa1);
check(plainWrite?.cs === null && plainWrite?.dc === null, "... a device without chip select nor DC pin shows none");

socket.close();
device.close();
report("spi");
