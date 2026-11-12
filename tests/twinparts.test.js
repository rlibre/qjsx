/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The parts a project wires on its twin ("qjsx agent -p
// tests/fixtures/twinparts": the "twin" section of its qjsx.json): a BME280
// (a model of qjsx) and an echo device (a model of the project, with a view
// of its own) on the I2C bus of pins 8 / 9, read by the program of the
// project with device/i2c; the page sets the temperature of the BME280; a
// potentiometer read with device/adc; the I2C trace of a bus with nothing on
// it.
//
//   qjsx run tests/twinparts.test.js tcp:127.0.0.1:<agent port> <page port>
import * as fs from "sys/fs";
import { connect as netConnect } from "sys/net";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND } from "qjsx:protocol";
import { connect } from "qjsx:websocket";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const PAGE_PORT = Number(process.argv[3]);
const PROJECT = "tests/fixtures/twinparts";
const TIMEOUT = 5000;

// --- the program's output, line by line
const lines = [];
const lineWaiters = [];

function onLog(stream, text) {
    for (const line of text.split("\n").filter(item => item !== "")) {
        lines.push(line);
        for (const waiter of lineWaiters.filter(item => item.line === line)) {
            lineWaiters.splice(lineWaiters.indexOf(waiter), 1);
            clearTimeout(waiter.timer);
            waiter.resolve(true);
        }
    }
}

// Resolves true when the program has printed 'line', false after TIMEOUT.
function printed(line) {
    if (lines.includes(line)) {
        return Promise.resolve(true);
    }
    return new Promise(resolve => {
        const waiter = { line, resolve, timer: undefined };
        waiter.timer = setTimeout(() => resolve(false), TIMEOUT);
        lineWaiters.push(waiter);
    });
}

// --- the messages of the page's WebSocket
const messages = [];
const messageWaiters = [];

function onPageMessage(text) {
    const message = JSON.parse(text);
    messages.push(message);
    for (const waiter of messageWaiters.filter(item => item.test(message))) {
        messageWaiters.splice(messageWaiters.indexOf(waiter), 1);
        clearTimeout(waiter.timer);
        waiter.resolve(message.params);
    }
}

// The params of the first message 'test(message)' accepts (received or to
// come); undefined after TIMEOUT.
function message(test) {
    const found = messages.find(test);
    if (found !== undefined) {
        return Promise.resolve(found.params);
    }
    return new Promise(resolve => {
        const waiter = { test, resolve, timer: undefined };
        waiter.timer = setTimeout(() => resolve(undefined), TIMEOUT);
        messageWaiters.push(waiter);
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

// --- the page: the parts of the board, then those of the project
const socket = await connect({ port: PAGE_PORT, path: "/ws", protocol: "messaging" });
socket.onMessage(onPageMessage);
const parts = await message(item => item.msg === "twin.parts");
equal(parts?.map(part => part.id).join(","), "boot,rgb,meteo,echo,strip,gain", "the page gets the board's parts then the project's");
const meteo = parts?.find(part => part.id === "meteo");
equal(meteo?.controls.map(control => control.type).join(","), "knob,knob,knob,value", "the bme280 declares its controls");
equal(JSON.stringify(meteo?.pins), "[8,9]", "... and is on the pins of its bus");
const echoPart = parts?.find(part => part.id === "echo");
equal(echoPart?.view, "/parts/echo/view.js", "the project's part has a view of its own");
const view = await get("/parts/echo/view.js");
check(view.head.includes("text/javascript") && view.body.includes("createView"), "... served by the twin");
const meteoState = await message(item => item.msg === "twin.part.state" && item.params.id === "meteo");
equal(meteoState?.state.temperature, 21.5, "the bme280 publishes its initial temperature");

// --- the program reads the parts
const device = await Device.open(TARGET, { onLog });
await device.hello();
const projectBuild = build(PROJECT);
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(projectBuild.file));
await device.request(KIND.RUN);
check(await printed("probe 0x76 true"), "a probe finds the bme280");
check(await printed("probe 0x51 false"), "... and nobody where no part is");
check(await printed("echo 3,2,1"), "the project's part answers the transfers (bytes reversed)");
check(await printed("chip id 0x60"), "the bme280 gives its chip id");
check(await printed("weather 21.50 C 40.0 % 1013.25 hPa"),
    "the program's driver reads the initial weather (calibration, forced measurement, compensation)");
const logged = await message(item => item.msg === "twin.log" && item.params.id === "echo");
equal(logged?.text, "transfer 3/3", "a part's log reaches the page");

// --- the page sets the temperature, the program reads it
socket.send(JSON.stringify({ msg: "twin.part.action", params: { id: "meteo", action: "set", args: { temperature: 30 } } }));
check(await printed("weather 30.00 C 40.0 % 1013.25 hPa"), "a temperature set on the page reaches the program");
socket.send(JSON.stringify({ msg: "twin.part.action", params: { id: "meteo", action: "set", args: { humidity: 65, pressure: 990 } } }));
check(await printed("weather 30.00 C 65.0 % 990.00 hPa"), "... and so do the humidity and the pressure");

// --- the potentiometer, read by device/adc
check(await printed("adc bits 12"), "device/adc gives the resolution of the board");
check(await printed("adc on GPIO21 EINVAL"), "a pin without analog input is refused (EINVAL)");
check(await printed("gain 1.65 V"), "the potentiometer at 50 % gives half the supply");
socket.send(JSON.stringify({ msg: "twin.part.action", params: { id: "gain", action: "set", args: { position: 20 } } }));
check(await printed("gain 0.66 V"), "turned to 20 % on the page, the program reads 0.66 V");
socket.send(JSON.stringify({ msg: "twin.part.action", params: { id: "gain", action: "set", args: { position: 100 } } }));
check(await printed("gain 3.10 V"), "at 100 % (3.3 V) it saturates at the span of the converter (3.1 V), as on the ESP32-S3");
const adcPins = await new Promise(resolve => setTimeout(() => resolve(get("/state")), 200));
const pin6 = JSON.parse(adcPins.body).pins[6];
check(pin6.adc === true && Math.abs(pin6.voltage - 3.3) < 0.001, "the page sees the analog input and the voltage on the pin");

// --- the I2C trace: the bus with nothing on it
const nack = await message(item => item.msg === "twin.i2c.data" && item.params.records.some(record => record.bus === 1));
const write = nack?.records.find(record => record.bus === 1 && record.writeSize === 2);
check(write?.status === "nack" && JSON.stringify(write?.written) === "[0,175]",
    "the page gets the I2C trace: the write to 0x3c on the empty bus, refused");

socket.close();
device.close();
report("twinparts");
