/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The digital twin ("qjsx agent --board yd-esp32-s3 --ui-port <port>"):
// what the agent says of itself, the board's native modules only, and the
// program of tests/fixtures/twin driven through the page (HTTP + WebSocket
// with the x4js messages of host/twin/ui), the parts of the board (their
// models: button, ws2812), the changes of the pins for the analyzer.
//
//   qjsx run tests/twin.test.js tcp:127.0.0.1:<agent port> <page port>
import * as fs from "sys/fs";
import { connect as netConnect } from "sys/net";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND, KEY } from "qjsx:protocol";
import { connect } from "qjsx:websocket";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const PAGE_PORT = Number(process.argv[3]);
const PROJECT = "tests/fixtures/twin";
const LINE_TIMEOUT = 5000;

// --- the program's output, line by line
const lines = [];
const lineWaiters = [];

function onLog(stream, text) {
    for (const line of text.split("\n").filter(item => item !== "")) {
        lines.push(line);
        for (const waiter of lineWaiters.filter(item => item.line === line)) {
            lineWaiters.splice(lineWaiters.indexOf(waiter), 1);
            clearTimeout(waiter.timer);
            waiter.resolve();
        }
    }
}

// Resolves when the program has printed 'line' (rejects after LINE_TIMEOUT).
function printed(line) {
    if (lines.includes(line)) {
        return Promise.resolve();
    }
    return new Promise((resolve, reject) => {
        const waiter = { line, resolve, timer: undefined };
        waiter.timer = setTimeout(() => reject(new Error(`the program did not print "${line}"`)), LINE_TIMEOUT);
        lineWaiters.push(waiter);
    });
}

// GET on the page's server: { head, body } as texts.
async function get(path) {
    const connection = await netConnect({ port: PAGE_PORT });
    const answer = await new Promise(done => {
        let text = "";
        connection.onData(bytes => {
            text += new TextDecoder().decode(bytes);
        });
        connection.onClose(() => done(text));
        connection.write(`GET ${path} HTTP/1.1\r\nHost: localhost\r\n\r\n`);
    });
    const headEnd = answer.indexOf("\r\n\r\n");
    return { head: answer.slice(0, headEnd), body: answer.slice(headEnd + 4) };
}

// GET on the page's server: the parsed JSON body.
async function getJson(path) {
    const { body } = await get(path);
    return JSON.parse(body);
}

// --- the messages of the page's WebSocket, as the x4js page receives them
const messages = [];
const messageWaiters = [];

// Keeps a message; settles the waits it answers.
function onPageMessage(text) {
    const message = JSON.parse(text);
    messages.push(message);
    for (const waiter of messageWaiters.filter(item => item.test(message))) {
        messageWaiters.splice(messageWaiters.indexOf(waiter), 1);
        clearTimeout(waiter.timer);
        waiter.resolve(message.params);
    }
}

// The params of the first message (already received, or to come) that
// 'test(message)' accepts; rejects after LINE_TIMEOUT.
function message(test, what) {
    const found = messages.find(test);
    if (found !== undefined) {
        return Promise.resolve(found.params);
    }
    return new Promise((resolve, reject) => {
        const waiter = { test, resolve, timer: undefined };
        waiter.timer = setTimeout(() => reject(new Error(`no message ${what}`)), LINE_TIMEOUT);
        messageWaiters.push(waiter);
    });
}

// The params of the first twin.part.state of part 'id' that 'test(state)'
// accepts.
function partState(id, test) {
    return message(item => item.msg === "twin.part.state" && item.params.id === id && test(item.params.state),
        `twin.part.state of "${id}"`);
}


// --- the agent: a twin of the board, with the board's native modules only
const device = await Device.open(TARGET, { onLog });
const hello = await device.hello();
equal(hello.get(KEY.AGENT), "qjsx twin", "the agent says it is a twin");
check(device.natives.includes("device/gpio") && device.natives.includes("device/rmt"),
    "the board's hardware modules are there");
check(!device.natives.includes("sys/net") && !device.natives.includes("sys/compiler"),
    "the PC's modules are not offered to the program");

// --- the page: the x4js application embedded in the executable
const page = await get("/");
check(page.head.startsWith("HTTP/1.1 200") && page.head.includes("text/html"), "/ serves the page");
check(page.body.includes('src="main.js"'), "... which loads main.js");
const script = await get("/main.js");
check(script.head.includes("text/javascript") && script.body.length > 1000, "/main.js is served from the embedded archive");
const missing = await get("/nothing.js");
check(missing.head.startsWith("HTTP/1.1 404"), "an unknown file is not found");

// --- the board on the page
const board = await getJson("/board.json");
equal(board.name, "YD-ESP32-S3", "/board.json gives the board");
const rest = await getJson("/state");
equal(rest.pins[0].level, 1, "BOOT (GPIO0) at rest reads 1: the board pulls it up");
equal(rest.pins[2].level, 0, "a free pin reads 0");

// --- the program
const projectBuild = build(PROJECT);
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(projectBuild.file));
await device.request(KIND.RUN);
await printed("waiting for boot");
check(lines.includes("platform esp32"), "process.platform is the board's");
check(lines.includes("escape: EACCES"), "the program reaches no file outside the data folder");
check(lines.includes("boot at rest 1"), "the program reads BOOT at 1");
check(lines.includes("output 1"), "an output reads what it drives");

const running = await getJson("/state");
equal(running.pins[2].configured, true, "the page sees the output configured");
equal(running.pins[2].mode, "output", "... as an output");
equal(running.pins[2].level, 1, "... at 1");
equal(running.pins[48].rmt, true, "the page sees the LED's pin driven by the RMT");

// --- the WebSocket of the page: x4js global messages, subprotocol "messaging"
const socket = await connect({ port: PAGE_PORT, path: "/ws", protocol: "messaging" });
socket.onMessage(onPageMessage);
const boardMessage = await message(item => item.msg === "twin.board", "twin.board");
equal(boardMessage.name, "YD-ESP32-S3", "a page gets twin.board when it connects");
const stateMessage = await message(item => item.msg === "twin.state", "twin.state");
equal(stateMessage.pins[2].level, 1, "... then twin.state");

// --- the parts of the board: their models (button, ws2812)
const parts = await message(item => item.msg === "twin.parts", "twin.parts");
equal(parts.map(part => `${part.id}:${part.model}`).join(","), "boot:button,rgb:ws2812", "a page gets the board's parts");
equal(JSON.stringify(parts.map(part => part.pins)), "[[0],[48]]", "... and the pins they are on");
equal(parts[0].controls[0]?.type, "button", "... with the controls their models declare");
const lit = await partState("rgb", state => state.colors?.[0]?.[0] !== 0).catch(() => undefined);
equal(JSON.stringify(lit?.state.colors), "[[10,20,30]]", "the ws2812 model decodes the color the program sent (GRB)");
equal(rest.pins[0].partPull, "up", "the button model pulls BOOT up");

// --- BOOT pressed: an action of the page to the button model
socket.send(JSON.stringify({ msg: "twin.part.action", params: { id: "boot", action: "set", args: { pressed: true } } }));
let pressed = true;
try {
    await printed("boot pressed");
} catch {
    pressed = false;
}
check(pressed, "pressing BOOT (the button model drives GPIO0 to 0) ends the program's waitFor(0)");
await partState("boot", state => state.pressed === true);
const held = await getJson("/state");
equal(held.pins[0].partDrive, 0, "... the page sees the part driving the pin");

socket.send(JSON.stringify({ msg: "twin.part.action", params: { id: "boot", action: "set", args: { pressed: false } } }));
const released = await new Promise(resolve => setTimeout(() => resolve(getJson("/state")), 200));
equal(released.pins[0].level, 1, "released, BOOT goes back to 1");
equal(released.pins[0].partDrive, null, "... and the part drives it no more");

// --- a pin forced by the page, then let go
socket.send(JSON.stringify({ msg: "twin.pin.force", params: { pin: 5, level: 1 } }));
const forced = await new Promise(resolve => setTimeout(() => resolve(getJson("/state")), 200));
equal(forced.pins[5].level, 1, "a pin forced to 1 by the page reads 1");
socket.send(JSON.stringify({ msg: "twin.pin.force", params: { pin: 5, level: -1 } }));
const free = await new Promise(resolve => setTimeout(() => resolve(getJson("/state")), 200));
equal(free.pins[5].forced, null, "... and let go, it is not forced any more");

// --- the analyzer: the changes of the pins with their time (twin.scope.data)
const scopeMessages = messages.filter(item => item.msg === "twin.scope.data").map(item => item.params);
check(scopeMessages.length > 0, "a page gets twin.scope.data");

// the changes of a pin in every message: records are [time, level, millivolts, ...]
function scopeChanges(pin) {
    const records = scopeMessages.flatMap(data => data.changes.filter(change => change.pin === pin).flatMap(change => change.records));
    const changes = [];
    for (let i = 0; i < records.length; i += 3) {
        changes.push({ time: records[i], level: records[i + 1], millivolts: records[i + 2] });
    }
    return changes;
}

const bootChanges = scopeChanges(0);
equal(bootChanges.map(change => change.level).join(","), "0,1", "the analyzer records BOOT pressed, then released");
check(bootChanges[1]?.time > bootChanges[0]?.time, "... in the order of their times");
equal(bootChanges.map(change => change.millivolts).join(","), "0,3300", "... with the voltage after each change");
equal(scopeChanges(5).map(change => change.level).join(","), "1,0", "... and the pin the page forced, then let go");
const lastScope = scopeMessages[scopeMessages.length - 1];
equal(lastScope.levels[0], 1, "... each message gives the level of every pin now");
equal(lastScope.millivolts[0], 3300, "... and its voltage");

socket.close();
device.close();
report("twin");
