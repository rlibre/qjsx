/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// stdlib/display and sys/draw in the digital twin of the WT32-SC01 ("qjsx
// agent -p tests/fixtures/twindisplay": board wt32-sc01, its screen the
// part st7796-320x480 on SPI2). The program of the project draws, step by
// step; this test plays a page of the twin: it asks the screen part for its
// whole glass, keeps the rectangles it publishes, and checks pixels of the
// glass (seen USB connector at the bottom, 320 x 480, 16 bits a pixel) after
// each step: rectangles, a line, a text; show(x, y, w, h) changing only that
// area; the four rotations; the backlight. It forces GPIO4 to let the
// program go on to its next step.
//
//   qjsx run tests/display.test.js tcp:127.0.0.1:<agent port> <page port>
import * as fs from "sys/fs";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND } from "qjsx:protocol";
import { connect } from "qjsx:websocket";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const PAGE_PORT = Number(process.argv[3]);
const PROJECT = "tests/fixtures/twindisplay";
const TIMEOUT = 8000;
const SETTLE_MS = 300;          // the part publishes every 50 ms: what changed is there after that
const WIDTH = 320;
const HEIGHT = 480;

// The 16 bits of the colors drawn.
const BLACK = 0x0000;
const WHITE = 0xffff;
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

// Resolves true once the program has printed 'line', false after TIMEOUT.
async function printed(line) {
    const end = Date.now() + TIMEOUT;
    while (!lines.includes(line) && Date.now() < end) {
        await new Promise(resolve => setTimeout(resolve, 20));
    }
    return lines.includes(line);
}

// The line the program printed that starts with 'start', or undefined.
function lineStarting(start) {
    return lines.find(line => line.startsWith(start));
}

// --- the glass as the page rebuilds it from what the part publishes
const glass = new Uint8Array(WIDTH * HEIGHT * 2);
let look = null;                // the last state, without its pixels
let publishedRects = 0;         // rectangles received
const messages = [];

function onPageMessage(text) {
    const message = JSON.parse(text);
    messages.push(message);
    const isScreen = message.msg === "twin.part.state" && message.params.id === "screen";
    if (!isScreen) {
        return;
    }

    // its rectangles copied into the glass, row by row
    const { rects, ...rest } = message.params.state;
    look = rest;
    publishedRects += rects.length;
    for (const rect of rects) {
        const pixels = Uint8Array.fromBase64(rect.pixels);
        for (let row = 0; row < rect.height; row++) {
            const from = row * rect.width * 2;
            glass.set(pixels.subarray(from, from + rect.width * 2), ((rect.y + row) * WIDTH + rect.x) * 2);
        }
    }
}

// The pixel of the glass at (x, y).
function pixel(x, y) {
    const offset = (y * WIDTH + x) * 2;
    return (glass[offset] << 8) | glass[offset + 1];
}

// The pixel as text, for the messages.
function hex(value) {
    return `0x${value.toString(16).padStart(4, "0")}`;
}

// Waits for what the part publishes after a step.
function settle() {
    return new Promise(resolve => setTimeout(resolve, SETTLE_MS));
}

// --- the page: the screen part of the board
const socket = await connect({ port: PAGE_PORT, path: "/ws", protocol: "messaging" });
socket.onMessage(onPageMessage);
await settle();
const parts = messages.find(item => item.msg === "twin.parts")?.params;
const screen = parts?.find(part => part.id === "screen");
equal(screen?.model, "st7796-320x480", "the board has its screen, a part of qjsx");
equal(JSON.stringify([...(screen?.pins ?? [])].sort((a, b) => a - b)), "[15,21,22,23]", "... on its chip select, DC, reset and backlight pins");

// the whole glass asked for, as a page does
socket.send(JSON.stringify({ msg: "twin.part.action", params: { id: "screen", action: "refresh" } }));

// the program goes on when GPIO4 is forced to the next level
let level = 0;

function nextStep() {
    level = 1 - level;
    socket.send(JSON.stringify({ msg: "twin.pin.force", params: { pin: 4, level } }));
}

// --- the program
const device = await Device.open(TARGET, { onLog });
await device.hello();
check(device.natives.includes("sys/draw"), "the twin of the WT32-SC01 offers sys/draw");
const projectBuild = build(PROJECT);
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(projectBuild.file));
await device.request(KIND.RUN);

// --- 1. rotation 0: rectangles, a line, a text
check(await printed("shown 1"), "the program showed its first image");
await settle();
equal(look?.turn, 0, "rotation 0: the image is not turned on the glass");
check(look?.awake === true && look?.on === true, "... the controller out of sleep, its display on");
equal(look?.brightness, 1, "... the backlight fully on (PWM duty 1)");
equal(hex(pixel(10, 20)), hex(RED), "the top left of the red rectangle, at (10, 20): origin top left");
equal(hex(pixel(39, 59)), hex(RED), "... its bottom right (30 x 40)");
equal(hex(pixel(40, 59)), hex(BLACK), "... nothing right of it: the background");
equal(hex(pixel(10, 60)), hex(BLACK), "... nothing under it");
equal(hex(pixel(100, 200)), hex(GREEN), "the green rectangle");
equal(hex(pixel(0, 479)), hex(BLUE), "the line on the last row, its first pixel");
equal(hex(pixel(319, 479)), hex(BLUE), "... and its last pixel");
equal(hex(pixel(200, 100)), hex(BLACK), "what no command covers is the background");

// the text: lit pixels in its box, none just left of it
const [, textWidth, lineHeight] = (lineStarting("text ") ?? "text 0 0").split(" ").map(Number);
let lit = 0;
for (let y = 300; y < 300 + lineHeight; y++) {
    for (let x = 50; x < 50 + textWidth; x++) {
        lit += pixel(x, y) !== BLACK ? 1 : 0;
    }
}
check(textWidth > 0 && lit > 20, `the text "Hi" lights pixels in its box (${lit} of ${textWidth} x ${lineHeight})`);
let leftOfText = 0;
for (let y = 300; y < 300 + lineHeight; y++) {
    leftOfText += pixel(49, y) !== BLACK ? 1 : 0;
}
equal(leftOfText, 0, "... and none left of it");

// the trace of the page: the area and the pixels of the whole screen went as
// ONE transaction (the commands of the area, then the made segment)
const traced = messages.filter(item => item.msg === "twin.spi.data").flatMap(item => item.params.records);
const imageWrite = traced.find(record => record.segments.some(segment => segment.size === WIDTH * HEIGHT * 2));
const pixelSegment = imageWrite?.segments.find(segment => segment.size === WIDTH * HEIGHT * 2);
check(imageWrite !== undefined && imageWrite.outcome === "answered" && pixelSegment?.kind === "write",
    "the trace has the made segment as a write of 307200 bytes, answered by the screen");
equal(pixelSegment?.written.length, 16, "... of which it keeps the first 16 bytes only");
const commands = (imageWrite?.segments ?? []).filter(segment => segment.dc === 0).map(segment => segment.written[0]);
equal(JSON.stringify(commands), "[42,43,44]", "... in the same transaction as the area: 0x2A, 0x2B, 0x2C");

// the state of the page: the bus with the screen on it (the page then does
// not show the bus: the screen shows itself)
const states = messages.filter(item => item.msg === "twin.state");
const spiBus = states[states.length - 1]?.params.spi.find(bus => bus.port === 2);
check(spiBus?.open === true && JSON.stringify(spiBus?.parts) === '[{"cs":15,"id":"screen"}]',
    "the state gives SPI2 open with the screen on chip select 15");

// nothing more is sent while nothing changes
const rectsBefore = publishedRects;
await settle();
equal(publishedRects, rectsBefore, "an unchanged screen sends no pixels");

// --- 2. white everywhere, but only the area (10, 20, 20, 10) shown
const before = glass.slice();
nextStep();
check(await printed("shown 2"), "the program showed an area");
await settle();
let changed = 0;
let outside = 0;
let notWhite = 0;
for (let y = 0; y < HEIGHT; y++) {
    for (let x = 0; x < WIDTH; x++) {
        const offset = (y * WIDTH + x) * 2;
        const inArea = x >= 10 && x < 30 && y >= 20 && y < 30;
        const differs = glass[offset] !== before[offset] || glass[offset + 1] !== before[offset + 1];
        changed += differs ? 1 : 0;
        outside += differs && !inArea ? 1 : 0;
        notWhite += inArea && pixel(x, y) !== WHITE ? 1 : 0;
    }
}
check(changed > 0 && outside === 0, `show(10, 20, 20, 10) changes only that area (${changed} pixels changed, ${outside} outside)`);
equal(notWhite, 0, "... all of it white");
equal(hex(pixel(30, 20)), hex(RED), "the red rectangle right of the area stays");
equal(hex(pixel(10, 30)), hex(RED), "... and under it");
equal(hex(pixel(0, 479)), hex(BLUE), "the line stays");

// --- 3. the other rotations: a red rectangle of 10 x 5 at the program's (0, 0)
const rotations = [
    { rotation: 90, size: "480x320", corners: [[319, 0], [315, 9]], outside: [[314, 0], [319, 10]] },
    { rotation: 180, size: "320x480", corners: [[319, 479], [310, 475]], outside: [[309, 479], [319, 474]] },
    { rotation: 270, size: "480x320", corners: [[0, 479], [4, 470]], outside: [[5, 479], [0, 469]] },
];

for (const { rotation, size, corners, outside: away } of rotations) {
    nextStep();
    check(await printed(`shown ${rotation} ${size}`), `rotation ${rotation}: the program sees a screen of ${size}`);
    await settle();
    equal(look?.turn, rotation, `... the image turned ${rotation} degrees on the glass`);
    const red = corners.every(([x, y]) => pixel(x, y) === RED);
    const black = away.every(([x, y]) => pixel(x, y) === BLACK);
    check(red && black, `... its (0, 0) and (9, 4) at glass ${JSON.stringify(corners)}, nothing beside`);
}
equal(hex(pixel(100, 200)), hex(BLACK), "the reset pin pulsed by each new Display cleared the screen");

// --- 4. the backlight at half
nextStep();
check(await printed("shown backlight"), "the program set the backlight");
await settle();
check(Math.abs((look?.brightness ?? 0) - 0.5) < 0.01, `backlight(0.5): the screen at half brightness (${look?.brightness})`);

// --- 5. values that change, in a proportional font: what the page rebuilt
// from the rectangles is what the screen holds (a full refresh changes
// nothing: no changed pixel was left unmarked)
nextStep();
check(await printed("shown values"), "the program showed 20 values");
await settle();
const rebuilt = glass.slice();
socket.send(JSON.stringify({ msg: "twin.part.action", params: { id: "screen", action: "refresh" } }));
await settle();
let stale = 0;
for (let index = 0; index < glass.length; index++) {
    stale += glass[index] !== rebuilt[index] ? 1 : 0;
}
equal(stale, 0, "the glass rebuilt from the changes equals the whole glass sent again");

// --- 6. an image imported with { type: "image" }: 8 x 6 at (100, 100), its
// first column transparent, its second white at half, the rest red, over
// blue; white at half over blue as the renderer blends it (coverage 129/256)
const WHITE_OVER_BLUE = (15 << 11) | (31 << 5) | 31;
nextStep();
check(await printed("shown image 8x6"), "the program showed an image of 8 x 6");
await settle();
equal(hex(pixel(100, 100)), hex(BLUE), "its transparent column shows the background");
equal(hex(pixel(101, 100)), hex(WHITE_OVER_BLUE), "its half transparent column is blended over it");
equal(hex(pixel(102, 100)), hex(RED), "its opaque pixels: red");
equal(hex(pixel(107, 105)), hex(RED), "... to its bottom right");
equal(hex(pixel(108, 100)), hex(BLUE), "right of it: the background");
equal(hex(pixel(102, 106)), hex(BLUE), "under it: the background");

// --- the end
nextStep();
check(await printed("display end"), "the program ended");
socket.send(JSON.stringify({ msg: "twin.pin.force", params: { pin: 4, level: -1 } }));

socket.close();
device.close();
report("display");
