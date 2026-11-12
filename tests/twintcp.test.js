/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The TCP links of the digital twin chosen from its page ("qjsx agent
// --board yd-esp32-s3" without --uart-port / --usb-port): nothing listens
// at first; the page turns a link on, a port in use gives the error and
// turns it off, a USB function waits for the device to start; the choices
// are kept in <storage>/twin.json and taken again at the next start.
//
//   qjsx run tests/twintcp.test.js set  <page port> <storage> <free port>   first run
//   qjsx run tests/twintcp.test.js kept <page port> <storage> <free port>   after a restart
//
// Ports used: <free port> (UART 1), + 1 (taken by this test, for UART 2),
// + 2 (USB function 0).
import * as fs from "sys/fs";
import { connect as netConnect, listen } from "sys/net";
import { connect } from "qjsx:websocket";
import { check, equal, report } from "./assert.js";

const MODE = process.argv[2];
const PAGE_PORT = Number(process.argv[3]);
const STORAGE = process.argv[4];
const UART_PORT = Number(process.argv[5]);
const TAKEN_PORT = UART_PORT + 1;
const USB_PORT = UART_PORT + 2;
const WAIT_TIMEOUT = 5000;

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

// --- the page: its state, and the message that sets a link
const socket = await connect({ port: PAGE_PORT, path: "/ws", protocol: "messaging" });
let state = null;
socket.onMessage(text => {
    const message = JSON.parse(text);
    if (message.msg === "twin.state") {
        state = message.params;
    }
});

function setLink(kind, index, port, on) {
    socket.send(JSON.stringify({ msg: "twin.tcp.listen", params: { kind, index, port, listen: on } }));
}

check(await until(() => state !== null), "the page gets the state");

if (MODE === "set") {
    // --- nothing at first
    const uart = state.uarts[1].tcp;
    equal(`${uart.port} ${uart.listen} ${uart.listening}`, "0 false false", "a UART does not listen at first");
    equal(state.usb.functions[0].tcp.listening, false, "a USB function does not listen at first");
    check(!fs.existsSync(`${STORAGE}/twin.json`), "nothing kept yet");

    // --- UART 1 on, a client connects
    setLink("uart", 1, UART_PORT, true);
    check(await until(() => state.uarts[1].tcp.listening && state.uarts[1].tcp.port === UART_PORT),
        "the page turns the link of UART 1 on");
    const client = await netConnect({ port: UART_PORT });
    check(await until(() => state.uarts[1].tcp.client), "... a client connects to it");
    client.close();

    // --- a port in use: the error, the link off
    const server = listen({ port: TAKEN_PORT }, connection => connection.close());
    setLink("uart", 2, TAKEN_PORT, true);
    check(await until(() => state.uarts[2].tcp.error !== ""), "a port in use gives an error");
    equal(state.uarts[2].tcp.error, `port ${TAKEN_PORT} in use`, "... which says so");
    equal(state.uarts[2].tcp.listen, false, "... and turns the listening off");
    server.close();

    // --- USB: asked, waiting for the device
    setLink("usb", 0, USB_PORT, true);
    check(await until(() => state.usb.functions[0].tcp.listen), "a USB function can be turned on");
    equal(state.usb.functions[0].tcp.listening, false, "... it listens only once the device starts");

    // --- kept
    check(await until(() => keptChoices().usb0?.listen === true), "the choices are kept in twin.json");
    const kept = keptChoices();
    equal(JSON.stringify(kept.uart1), `{"port":${UART_PORT},"listen":true}`, "... UART 1 on");
    equal(JSON.stringify(kept.uart2), `{"port":${TAKEN_PORT},"listen":false}`, "... UART 2 off");
    equal(JSON.stringify(kept.usb0), `{"port":${USB_PORT},"listen":true}`, "... USB function 0 on");
} else {
    // --- after a restart: the choices taken again
    check(await until(() => state.uarts[1].tcp.listening), "after a restart, UART 1 listens again");
    equal(state.uarts[1].tcp.port, UART_PORT, "... on its port");
    equal(state.uarts[2].tcp.listen, false, "... UART 2 stays off");
    equal(state.usb.functions[0].tcp.port, USB_PORT, "... USB function 0 keeps its port");

    // --- off again
    setLink("uart", 1, UART_PORT, false);
    check(await until(() => !state.uarts[1].tcp.listening), "the page turns the link off");
    check(await until(() => keptChoices().uart1?.listen === false), "... and that is kept");
}

socket.close();
report(`twin tcp (${MODE})`);
