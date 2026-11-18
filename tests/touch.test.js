/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// stdlib/touch and the touch parts of the twin ("qjsx agent -p
// tests/fixtures/twintouch": the WT32-SC01 and its FT6336, plus a GT911 of
// the project). This test plays a page: it touches the screen by the actions
// the page sends to the touch parts ("press", "move", "release" in pixels of
// the glass) and checks what the program's touches report: the coordinates
// at rotation 0, at rotation 90 through the range of a GT911, with the
// mounting undone (swapXY, mirrorX), the threshold of a move, the rate of
// the reads.
//
//   qjsx run tests/touch.test.js tcp:127.0.0.1:<agent port> <page port>
import * as fs from "sys/fs";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND } from "qjsx:protocol";
import { connect } from "qjsx:websocket";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const PAGE_PORT = Number(process.argv[3]);
const PROJECT = "tests/fixtures/twintouch";
const TIMEOUT = 5000;
const QUIET_MS = 300;           // long enough for several reads: nothing printed means no change

// --- the program's output, line by line
const lines = [];

function onLog(stream, text) {
    for (const line of text.split("\n").filter(item => item !== "")) {
        lines.push(line);
    }
}

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

// Resolves true once the program has printed 'line' after the line 'from',
// false after TIMEOUT.
async function printedAfter(from, line) {
    const end = Date.now() + TIMEOUT;
    while (!lines.slice(from).includes(line) && Date.now() < end) {
        await sleep(20);
    }
    return lines.slice(from).includes(line);
}

// The lines printed after 'from' that start with 'start'.
function linesAfter(from, start) {
    return lines.slice(from).filter(line => line.startsWith(start));
}

// --- the page: the touch parts
const socket = await connect({ port: PAGE_PORT, path: "/ws", protocol: "messaging" });
const messages = [];
socket.onMessage(text => messages.push(JSON.parse(text)));
await sleep(300);
const parts = messages.find(item => item.msg === "twin.parts")?.params ?? [];
equal(parts.find(part => part.id === "touch")?.model, "ft6336", "the board has its touch panel, an FT6336");
equal(parts.find(part => part.id === "gt")?.model, "gt911", "the project adds a GT911");

// sends an action of the page to a part
function act(id, action, args) {
    socket.send(JSON.stringify({ msg: "twin.part.action", params: { id, action, args } }));
}

// --- the program
const device = await Device.open(TARGET, { onLog });
await device.hello();
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(build(PROJECT).file));
await device.request(KIND.RUN);
check(await printedAfter(0, "ready 2 5"), "the program opened the three touches: FT6336 2 points, GT911 5 (its configuration)");

// --- the FT6336 at rotation 0, and the same with its mounting undone
let from = lines.length;
act("touch", "press", { x: 100, y: 200, width: 320, height: 480 });
check(await printedAfter(from, "ft 1 100,200"), "a press: one finger where the glass was touched (rotation 0)");
check(await printedAfter(from, "mounted 1 119,100"), "... read with swapXY and mirrorX: x and y exchanged, then x mirrored");

from = lines.length;
act("touch", "move", { x: 101, y: 200, width: 320, height: 480 });
await sleep(QUIET_MS);
equal(linesAfter(from, "ft ").length, 0, "a move of 1 pixel is under the threshold: no change");

from = lines.length;
act("touch", "move", { x: 110, y: 205, width: 320, height: 480 });
check(await printedAfter(from, "ft 1 110,205"), "a move of 10 pixels is reported");
check(await printedAfter(from, "mounted 1 114,110"), "... and through the mounting");

from = lines.length;
act("touch", "release");
check(await printedAfter(from, "ft 0 -"), "the release: no finger");
check(await printedAfter(from, "mounted 0 -"), "... for the polled touch too");

// --- the rate: a finger moving 3 pixels every 5 ms for a second gives at
// most 'rate' (33) changes a second, not one per move
from = lines.length;
act("touch", "press", { x: 0, y: 100, width: 320, height: 480 });
for (let step = 1; step <= 200; step++) {
    act("touch", "move", { x: (step * 3) % 300, y: 100, width: 320, height: 480 });
    await sleep(5);
}
const moves = linesAfter(from, "ft 1 ").length;
check(moves >= 10 && moves <= 40, `200 moves in about a second give ${moves} changes (rate 33 a second)`);
act("touch", "release");
check(await printedAfter(from, "ft 0 -"), "... then the release");

// --- the GT911 of the project at rotation 90: range 640 x 960, so the
// glass's (100, 200) is (200, 400) on the chip, then (100, 200) of the panel
// again, turned: x = row, y = 319 - column
from = lines.length;
act("gt", "press", { x: 100, y: 200, width: 320, height: 480 });
check(await printedAfter(from, "gt 1 200,219"), "the GT911: scaled to its range and back, turned by the rotation 90");
equal(linesAfter(from, "ft ").length, 0, "... the FT6336 saw nothing");

from = lines.length;
act("gt", "move", { x: 120, y: 200, width: 320, height: 480 });
check(await printedAfter(from, "gt 1 200,199"), "its moves are reported (a report every 10 ms, read at the program's rate)");

from = lines.length;
act("gt", "release");
check(await printedAfter(from, "gt 0 -"), "its release: the last report, without point");

// two clicks of the mouse (pressed and released at once, twice): two
// touches, each with its release, none lost
from = lines.length;
for (let click = 0; click < 2; click++) {
    act("gt", "press", { x: 50, y: 60, width: 320, height: 480 });
    act("gt", "release");
    await sleep(200);
}
await sleep(QUIET_MS);
equal(linesAfter(from, "gt ").join(" | "), "gt 1 60,269 | gt 0 - | gt 1 60,269 | gt 0 -", "two quick clicks: two touches, two releases");

// --- the end
await device.request(KIND.STOP);
socket.close();
device.close();
report("touch");
