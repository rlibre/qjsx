/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// device/serial in the digital twin ("qjsx agent --board yd-esp32-s3
// --uart-port <port>"): the UARTs of the board, each a TCP port and a panel
// of the page. The program tests/fixtures/twinserial prints what the wrong
// calls give, then echoes on UART 1; this test plays the other end through
// the TCP port of UART 1 and through the WebSocket of the page.
//
//   qjsx run tests/serial.test.js tcp:127.0.0.1:<agent port> <uart port> <page port>
import * as fs from "sys/fs";
import { connect as netConnect } from "sys/net";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND } from "qjsx:protocol";
import { connect } from "qjsx:websocket";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const UART_PORT = Number(process.argv[3]);
const PAGE_PORT = Number(process.argv[4]);
const PROJECT = "tests/fixtures/twinserial";
const WAIT_TIMEOUT = 8000;

// --- the program's output, line by line
const lines = [];

function onLog(stream, text) {
    for (const line of text.split("\n").filter(item => item !== "")) {
        lines.push(line);
    }
}

// Resolves true once 'test()' is true, false after WAIT_TIMEOUT.
async function until(test) {
    const end = Date.now() + WAIT_TIMEOUT;
    while (!test() && Date.now() < end) {
        await new Promise(resolve => setTimeout(resolve, 20));
    }
    return test();
}

// The text the program printed after "<label>: ".
function answer(label) {
    const line = lines.find(item => item.startsWith(`${label}: `));
    return line === undefined ? undefined : line.slice(label.length + 2);
}

// --- the agent: the twin offers device/serial
const device = await Device.open(TARGET, { onLog });
await device.hello();
check(device.natives.includes("device/serial"), "the twin offers device/serial");

// --- the TCP client of UART 1, connected before the program opens it
const decoder = new TextDecoder();
let tcpText = "";
const client = await netConnect({ port: UART_PORT + 1 });
client.onData(bytes => {
    tcpText += decoder.decode(bytes);
});

// --- the page: its state and the traffic of the UARTs
const socket = await connect({ port: PAGE_PORT, path: "/ws", protocol: "messaging" });
let state = null;
const records = [];
socket.onMessage(text => {
    const message = JSON.parse(text);
    if (message.msg === "twin.state") {
        state = message.params;
    }
    if (message.msg === "twin.uart.data" && message.params.uart === 1) {
        records.push(...message.params.records);
    }
});

// --- the program
const projectBuild = build(PROJECT);
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(projectBuild.file));
await device.request(KIND.RUN);
check(await until(() => lines.includes("serial ready")), "the program opened UART 1");

// --- what the board has, what is refused
equal(answer("list"), "1,2", "list(): the free UARTs, not the agent's link");
equal(answer("link uart"), "EBUSY", "the UART of the agent's link is refused");
equal(answer("no such uart"), "ENOENT", "a UART the board does not have");
equal(answer("named port"), "TypeError", "a board's port is a number");
equal(answer("no pins"), "TypeError", "tx or rx is required");
equal(answer("rts alone"), "TypeError", "rts and cts go together");
equal(answer("flowControl"), "TypeError", "on a board, the flow control is the pins");
equal(answer("dataBits 9"), "RangeError", "dataBits is 5 to 8");
equal(answer("port"), "1", "port() gives the UART's number");
equal(answer("taken"), "EBUSY", "a UART open already is refused");
equal(answer("list open"), "2", "list() leaves out an open UART");

// --- the page sees the UART
check(await until(() => state?.uarts?.[1]?.open === true), "the page sees UART 1 open");
const uart = state.uarts[1];
equal(`${uart.baudRate} ${uart.dataBits}${uart.parity} ${uart.stopBits}`, "9600 8even 1", "... with its format");
equal(`${uart.tx} ${uart.rx} ${uart.rts}`, "17 18 null", "... and its pins");
equal(uart.tcp.client, true, "... and the TCP client");
equal(state.uarts[0].link, true, "the page sees UART 0 as the agent's link");

// --- TCP: the program answers
client.write("hello");
check(await until(() => tcpText.includes("echo:hello")), "TCP: the program answers what the client sends");

// --- the page sends: the program answers, the TCP client gets it too
socket.send(JSON.stringify({ msg: "twin.uart.send", params: { uart: 1, bytes: [0x61, 0x62, 0x63] } }));
check(await until(() => tcpText.includes("echo:abc")), "the page sends to the program, the answer goes to TCP");
const sent = record => decoder.decode(new Uint8Array(record.bytes));
check(await until(() => records.some(record => record.source === "page" && sent(record) === "abc")),
    "the page gets the record of what it sent");
check(records.some(record => record.source === "tcp" && sent(record) === "hello"), "... of what TCP sent");
check(await until(() => records.some(record => record.source === "program" && sent(record) === "echo:abc")),
    "... of what the program wrote");

// --- closing, then opening again at once
client.write("close");
check(await until(() => lines.includes("reopened: true")), "a closed UART opens again at once");
await new Promise(resolve => setTimeout(resolve, 100));
client.write("again");
check(await until(() => tcpText.includes("echo:again")), "... and works");

// --- the end
client.write("quit");
check(await until(() => lines.includes("serial done")), "the program ends");

client.close();
socket.close();
device.close();
report("serial");
