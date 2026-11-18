/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The twin of the M5Stack Tab5 ("qjsx agent -p tests/fixtures/twintab5":
// board m5stack-tab5, its display built in the part ili9881c-720x1280, its
// touch a gt911). The program draws with stdlib/display type "builtin" at
// rotation 0 then 90; this test plays a page: it keeps the rectangles the
// screen part publishes and checks pixels of the image (720 x 1280 as the
// panel scans it), the turn, the backlight; then it touches the screen and
// checks the point the program reads.
//
//   qjsx run tests/tab5.test.js tcp:127.0.0.1:<agent port> <page port>
import * as fs from "sys/fs";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND } from "qjsx:protocol";
import { connect } from "qjsx:websocket";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const PAGE_PORT = Number(process.argv[3]);
const PROJECT = "tests/fixtures/twintab5";
const TIMEOUT = 8000;
const SETTLE_MS = 300;          // the part sends every 66 ms: what changed is there after that
const WIDTH = 720;
const HEIGHT = 1280;

// The 16 bits of the colors drawn.
const BLACK = 0x0000;
const RED = 0xf800;
const GREEN = 0x07e0;
const BLUE = 0x001f;

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

// --- the image as the page rebuilds it from what the part publishes
const image = new Uint8Array(WIDTH * HEIGHT * 2);
let look = null;                // the last state, without its pixels
const messages = [];

function onPageMessage(text) {
    const message = JSON.parse(text);
    messages.push(message);
    const isScreen = message.msg === "twin.part.state" && message.params.id === "screen";
    if (!isScreen) {
        return;
    }

    // its rectangles copied into the image, row by row
    const { rects, ...rest } = message.params.state;
    look = rest;
    for (const rect of rects) {
        const bytes = Uint8Array.fromBase64(rect.pixels);
        for (let row = 0; row < rect.height; row++) {
            const from = row * rect.width * 2;
            image.set(bytes.subarray(from, from + rect.width * 2), ((rect.y + row) * WIDTH + rect.x) * 2);
        }
    }
}

// Resolves true once 'condition()' holds, false after TIMEOUT (a whole image
// is 2.4 MB of base64: it takes a while to come and to decode).
async function until(condition) {
    const end = Date.now() + TIMEOUT;
    while (!condition() && Date.now() < end) {
        await sleep(20);
    }
    return condition();
}

// The 16 bits of pixel (x, y) of the image.
function pixel(x, y) {
    const at = (y * WIDTH + x) * 2;
    return (image[at] << 8) | image[at + 1];
}

function hex(value) {
    return `0x${value.toString(16).padStart(4, "0")}`;
}

// --- the page
const socket = await connect({ port: PAGE_PORT, path: "/ws", protocol: "messaging" });
socket.onMessage(onPageMessage);
await sleep(SETTLE_MS);
const parts = messages.find(item => item.msg === "twin.parts")?.params ?? [];
equal(parts.find(part => part.id === "screen")?.model, "ili9881c-720x1280", "the board has its display built in");
equal(parts.find(part => part.id === "touch")?.model, "gt911", "... and its touch, a GT911");
socket.send(JSON.stringify({ msg: "twin.part.action", params: { id: "screen", action: "refresh" } }));

// --- the program
const device = await Device.open(TARGET, { onLog });
await device.hello();
check(device.natives.includes("sys/draw"), "the twin of the Tab5 offers sys/draw");
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(build(PROJECT).file));
await device.request(KIND.RUN);

// --- rotation 0: the whole screen
check(await printedAfter(0, "shown 0 720x1280"), "the program showed the whole screen at rotation 0 (720 x 1280)");
check(await until(() => pixel(10, 20) === RED), "the page got its pixels");
equal(look?.turn, 0, "rotation 0: the image is not turned");
equal(look?.brightness, 1, "the backlight fully on (PWM duty 1)");
equal(hex(pixel(10, 20)), hex(RED), "the top left of the red rectangle, at (10, 20)");
equal(hex(pixel(39, 59)), hex(RED), "... its bottom right (30 x 40)");
equal(hex(pixel(40, 59)), hex(BLACK), "... the background right of it");
equal(hex(pixel(719, 1279)), hex(GREEN), "the green rectangle in the bottom right corner");
equal(hex(pixel(360, 640)), hex(BLACK), "the background in the middle");

// --- rotation 90: drawing (x, y) is pixel (719 - y, x) of the image
check(await printedAfter(0, "shown 90 1280x720"), "the program showed an area at rotation 90 (1280 x 720)");
check(await until(() => look?.turn === 90), "the page got it");
equal(hex(pixel(719, 0)), hex(BLUE), "drawing (0, 0) is pixel (719, 0)");
equal(hex(pixel(710, 49)), hex(BLUE), "drawing (49, 9) is pixel (710, 49)");
equal(hex(pixel(709, 0)), hex(BLACK), "drawing (0, 10), outside the rectangle, kept black");
equal(hex(pixel(10, 20)), hex(RED), "outside the area shown, the image keeps what it had");

// --- the touch, in the coordinates of the wide display: the panel's (100,
// 200) is drawing (200, 719 - 100)
check(await printedAfter(0, "touch ready 10"), "the program opened the GT911: 10 points (its configuration)");
const from = lines.length;
socket.send(JSON.stringify({ msg: "twin.part.action", params: { id: "touch", action: "press", args: { x: 100, y: 200, width: WIDTH, height: HEIGHT } } }));
check(await printedAfter(from, "touch 1 200,619"), "a press on the panel at (100, 200): drawing (200, 619)");
socket.send(JSON.stringify({ msg: "twin.part.action", params: { id: "touch", action: "release" } }));
check(await printedAfter(from, "touch 0 -"), "... then its release");

// --- the end
await device.request(KIND.STOP);
socket.close();
device.close();
report("tab5");
