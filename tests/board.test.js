/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// device/board in the digital twin: the program tests/fixtures/boardinfo
// prints the id and the MAC addresses of the board, then restarts once. The
// id is the one kept in the storage of the agent (board-id), the addresses
// are derived from it, nothing of the program runs after restart(), and the
// installed program runs again.
//
//   qjsx run -A tests/board.test.js tcp:127.0.0.1:<agent port> <storage of the agent>
import * as fs from "sys/fs";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KEY, KIND } from "qjsx:protocol";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const STORAGE = process.argv[3];
const PROJECT = "tests/fixtures/boardinfo";
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

// The texts the program printed after "<label>: ", in order.
function answers(label) {
    return lines.filter(item => item.startsWith(`${label}: `)).map(item => item.slice(label.length + 2));
}

// 'hex' (12 digits) plus 'offset', as a 48-bit number, in 12 digits.
function addToMac(hex, offset) {
    return (BigInt(`0x${hex}`) + BigInt(offset)).toString(16).padStart(12, "0");
}

// --- the program, installed in the twin (a restart runs the installed one)
const device = await Device.open(TARGET, { onLog });
await device.hello();
check(device.natives.includes("device/board"), "the twin offers device/board");
fs.rmSync(`${STORAGE}/data/restarted`, { force: true });
const projectBuild = build(PROJECT);
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(projectBuild.file));
await device.request(KIND.RUN);

// --- the id: the one of the storage, the same after the restart
check(await printed("restarting"), "the program asked for a restart");
check(await printed("restarted"), "the installed program ran again after restart()");
const ids = answers("id");
equal(ids.length, 2, "the id printed by each run");
const kept = fs.readFileSync(`${STORAGE}/board-id`, "utf8").trim().toLowerCase();
equal(ids[0], kept, "the id is the one kept in the storage (board-id)");
equal(ids[1], ids[0], "the same id after the restart");
check(/^[0-9a-f]{12}$/.test(kept) && (parseInt(kept.slice(0, 2), 16) & 0x03) === 0x02, "a unicast, locally administered address");

// --- the MAC addresses, derived from it
equal(answers("mac base")[0], kept, "mac('base') is the id");
equal(answers("mac wifi")[0], kept, "mac('wifi') is the base");
equal(answers("mac ap")[0], addToMac(kept, 1), "mac('ap') is base + 1");
equal(answers("mac bluetooth")[0], addToMac(kept, 2), "mac('bluetooth') is base + 2");
equal(answers("mac ethernet")[0], addToMac(kept, 3), "mac('ethernet') is base + 3");
equal(answers("mac radio")[0], "TypeError", "an unknown kind is a TypeError");

// --- memory(): the JS heap measured; the PC's RAM is not the board's
const memoryReport = JSON.parse(answers("memory")[0] ?? "{}");
check(memoryReport.jsHeap > 0, `memory().jsHeap is measured (${memoryReport.jsHeap})`);
check(memoryReport.jsHeapPeak >= memoryReport.jsHeap, "... jsHeapPeak at least jsHeap");
equal(memoryReport.ramFree, null, "... ramFree null in the twin (the PC's RAM says nothing)");
equal(memoryReport.psramFree, null, "... psramFree null");
check("jsHeapSize" in memoryReport && "ramLargest" in memoryReport && "ramLowest" in memoryReport, "... every field there");

// --- load(): 200 ms of work and 300 ms of wait since the start (less than
// a second: the share of that time)
const loadReport = JSON.parse(answers("load")[0] ?? "{}");
check(loadReport.busy >= 200 && loadReport.elapsed >= 500, `load(): busy and elapsed in ms (${loadReport.busy}, ${loadReport.elapsed})`);
check(loadReport.js > 0.25 && loadReport.js < 0.9, `... js is the share of the time worked (${loadReport.js})`);
equal(loadReport.jsPeak, 0, "... no full second yet: no peak");

// --- INFO gives the load too
const info = await device.request(KIND.INFO);
check(info.get(KEY.JS_LOAD) >= 0 && info.get(KEY.JS_LOAD) <= 1000, "INFO: the load of the JS thread, in thousandths");
check(info.get(KEY.JS_LOAD_PEAK) !== undefined, "INFO: its peak");

// --- nothing ran after restart()
check(!lines.includes("after restart"), "nothing runs after restart()");
check(!lines.includes("finally ran"), "not even a finally");

device.close();
fs.rmSync(`${PROJECT}/build`, { recursive: true, force: true });
report("board");
