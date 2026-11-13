/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The wiring of the twin's UARTs to PC serial ports ("qjsx agent -p
// tests/fixtures/twinwire": its qjsx.json wires UART 2 to a port that does
// not exist). No PC port is needed: what is checked is the state the page
// sees, the errors of a port that cannot open, and what twin.json keeps
// (the page's choice of UART 1, next to its TCP link; nothing for the fixed
// UART 2). The bytes through a real port are not tested here (a USB serial
// adapter with TX wired to RX, by hand).
//
//   qjsx run tests/twinwire.test.js tcp:127.0.0.1:<agent port> <page port> <storage> <free port>
import * as fs from "sys/fs";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND } from "qjsx:protocol";
import { connect } from "qjsx:websocket";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const PAGE_PORT = Number(process.argv[3]);
const STORAGE = process.argv[4];
const FREE_PORT = Number(process.argv[5]);
const PROJECT = "tests/fixtures/twinwire";
const FIXED_PORT = "QJSX-NO-SUCH-PORT";      // "twin.uarts" of its qjsx.json, for UART 2
const MISSING_PORT = "QJSX-ALSO-MISSING";     // chosen on the page for UART 1
const WAIT_TIMEOUT = 8000;

// Resolves true once 'test()' is true, false after WAIT_TIMEOUT.
async function until(test) {
    const end = Date.now() + WAIT_TIMEOUT;
    while (!test() && Date.now() < end) {
        await new Promise(resolve => setTimeout(resolve, 20));
    }
    return test();
}

// The choices kept by the twin.
function keptChoices() {
    return JSON.parse(new TextDecoder().decode(fs.readFileSync(`${STORAGE}/twin.json`)));
}

// --- the program's output
const lines = [];
const device = await Device.open(TARGET, {
    onLog: (stream, text) => lines.push(...text.split("\n").filter(line => line !== "")),
});
await device.hello();

// --- the page: its state
const socket = await connect({ port: PAGE_PORT, path: "/ws", protocol: "messaging" });
let state = null;
socket.onMessage(text => {
    const message = JSON.parse(text);
    if (message.msg === "twin.state") {
        state = message.params;
    }
});
check(await until(() => state !== null), "the page gets the state");
check(Array.isArray(state.serialPorts), "... with the PC's serial ports");

// --- before the program: UART 2 wired by qjsx.json, UART 1 not
const fixed = state.uarts[2].serial;
equal(`${fixed.port} ${fixed.connect} ${fixed.fixed} ${fixed.realSpeed}`, `${FIXED_PORT} true true true`,
    "qjsx.json wires UART 2 from the start, at real speed, fixed");
equal(fixed.connected, false, "... not connected while the program has not opened it");
const free = state.uarts[1].serial;
equal(`${free.port}|${free.connect}|${free.fixed}|${free.realSpeed}`, "|false|false|true", "UART 1 is not wired");
equal(state.uarts[0].serial.port, "", "the agent's link has no wiring");

// --- the program opens UARTs 1 and 2: the fixed port cannot open
const projectBuild = build(PROJECT);
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(projectBuild.file));
await device.request(KIND.RUN);
check(await until(() => lines.includes("uarts open")), "the program opened UARTs 1 and 2");
check(await until(() => state.uarts[2].serial.error !== ""), "the port of UART 2 cannot open");
equal(state.uarts[2].serial.error, "no such port", "... the page says why");
equal(state.uarts[2].serial.connect, false, "... and the wiring is turned off");

// --- the page wires UART 1 to a port that does not exist, immediate speed
socket.send(JSON.stringify({ msg: "twin.uart.serial", params: { uart: 1, port: MISSING_PORT, connect: true, realSpeed: false } }));
check(await until(() => state.uarts[1].serial.error === "no such port"), "a port chosen on the page that does not exist: its error");
const chosen = state.uarts[1].serial;
equal(`${chosen.port} ${chosen.connect} ${chosen.realSpeed} ${chosen.fixed}`, `${MISSING_PORT} false false false`,
    "... the port and the speed kept, the wiring off");

// --- a bad message is ignored
socket.send(JSON.stringify({ msg: "twin.uart.serial", params: { uart: 1, port: 5, connect: true, realSpeed: true } }));
await new Promise(resolve => setTimeout(resolve, 200));
equal(state.uarts[1].serial.port, MISSING_PORT, "a message with a wrong port is ignored");

// --- the TCP link of UART 1 too: both kept in twin.json
socket.send(JSON.stringify({ msg: "twin.tcp.listen", params: { kind: "uart", index: 1, port: FREE_PORT, listen: true } }));
check(await until(() => state.uarts[1].tcp.listening), "the TCP link of the same UART listens too");
check(await until(() => keptChoices().uart1 !== undefined), "twin.json keeps the TCP link");
const kept = keptChoices();
equal(JSON.stringify(kept.serial1), `{"port":"${MISSING_PORT}","connect":false,"realSpeed":false}`,
    "... and the wiring chosen on the page, next to it");
equal(kept.serial2, undefined, "... not the wiring fixed by qjsx.json");

// --- the end
socket.send(JSON.stringify({ msg: "twin.uart.send", params: { uart: 1, bytes: Array.from(new TextEncoder().encode("quit")) } }));
check(await until(() => lines.includes("wire done")), "the program ends");

socket.close();
device.close();
report("twinwire");
