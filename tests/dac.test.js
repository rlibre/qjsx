/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// device/dac in the digital twin of an original ESP32 ("qjsx agent -p
// tests/fixtures/twindac": board esp32-devkitc-wroom32, two 8-bit DACs on
// GPIO25 / GPIO26, 3.3 V full scale; no part). The program of the project
// prints what each call gives; this test checks its lines, then the pins as
// the page's server gives them (what the meters of the page show), and the
// outputs stopped by close() and at the end of the program.
//
//   qjsx run tests/dac.test.js tcp:127.0.0.1:<agent port> <page port>
import * as fs from "sys/fs";
import { connect as netConnect } from "sys/net";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND } from "qjsx:protocol";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const PAGE_PORT = Number(process.argv[3]);
const PROJECT = "tests/fixtures/twindac";
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

// The pins as the page's server gives them (GET /state).
async function pins() {
    const connection = await netConnect({ port: PAGE_PORT });
    const text = await new Promise(done => {
        let received = "";
        connection.onData(bytes => {
            received += new TextDecoder().decode(bytes);
        });
        connection.onClose(() => done(received));
        connection.write("GET /state HTTP/1.1\r\nHost: localhost\r\n\r\n");
    });
    return JSON.parse(text.slice(text.indexOf("\r\n\r\n") + 4)).pins;
}

// Resolves with the pins once 'test' holds for them, null after WAIT_TIMEOUT.
async function pinsWhen(test) {
    const end = Date.now() + WAIT_TIMEOUT;
    while (Date.now() < end) {
        const state = await pins();
        if (test(state)) {
            return state;
        }
        await new Promise(resolve => setTimeout(resolve, 50));
    }
    return null;
}

// --- the agent: the twin of an original ESP32 offers device/dac
const device = await Device.open(TARGET, { onLog });
await device.hello();
check(device.natives.includes("device/dac"), "the twin of an original ESP32 offers device/dac");
check(!device.natives.includes("device/usb"), "... and not device/usb (no USB OTG)");

// --- before the program: nothing drives the outputs
const before = await pins();
equal(before[26].dac, false, "no DAC drives GPIO26 yet");
equal(before[26].voltage, 0, "... the pin at 0 V");

// --- the program
const projectBuild = build(PROJECT);
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(projectBuild.file));
await device.request(KIND.RUN);
check(await printed("dac ready"), "the program opened its outputs");

// --- an output, its first code, what the hardware has
equal(answer("opened"), "8 3.3 64", "8 bits, 3.3 V full scale, the first code given");
equal(answer("write"), "200", "write() drives a code, value follows");

// --- codes: integers 0 to 255 only
equal(answer("write 257"), "RangeError", "a code above 255 is refused");
equal(answer("write 256"), "RangeError", "256 is above 8 bits");
equal(answer("write 1.5"), "RangeError", "a fraction is not a code (never truncated)");
equal(answer("write -1"), "RangeError", "a negative code is refused");
equal(answer("write NaN"), "RangeError", "NaN is not a code");
equal(answer("write text"), "TypeError", "a text is not a number");
equal(answer("value kept"), "200", "a refused write changes nothing");

// --- voltages: rounded to the nearest code
equal(answer("1.2 V"), "93", "1.2 V is code 92.7, rounded to 93");
equal(answer("2 V"), "155", "2 V is code 154.5, rounded to 155");
equal(answer("3.3 V"), "255", "the full scale is the highest code");
equal(answer("0 V"), "0", "0 V is code 0");
equal(answer("voltage above"), "RangeError", "above the full scale is refused");
equal(answer("voltage negative"), "RangeError", "a negative voltage is refused");
equal(answer("voltage NaN"), "RangeError", "NaN is not a voltage");
equal(answer("voltage text"), "TypeError", "a text is not a voltage");

// --- refused
equal(answer("no dac"), "EINVAL", "a pin without DAC");
equal(answer("taken"), "EBUSY", "a DAC open already");
equal(answer("pin fraction"), "RangeError", "a pin that is not an integer");
equal(answer("pin text"), "TypeError", "a pin that is not a number");
equal(answer("bad value"), "RangeError", "a first code above 255");
equal(answer("unknown option"), "TypeError", "an unknown option");

// --- closed
equal(answer("closed write"), "EBADF", "a closed output refuses write()");
equal(answer("closed voltage"), "EBADF", "... and writeVoltage()");
equal(answer("closed value"), "10", "value keeps the last code after close()");
equal(answer("second"), "93", "the refused first code left GPIO26 free");

// --- the pins on the page's server
const running = await pins();
equal(running[25].dac, true, "the page sees the DAC of GPIO25");
equal(running[25].voltage.toFixed(3), "1.656", "code 128 drives 128 / 255 of 3.3 V");
equal(running[25].level, 1, "above half of 3.3 V the pin reads 1");
equal(running[26].dac, true, "the page sees the DAC of GPIO26");
equal(running[26].voltage.toFixed(3), "1.204", "code 93 drives 93 / 255 of 3.3 V");
equal(running[26].level, 0, "below half of 3.3 V the pin reads 0");
equal(running[4].dac, false, "a pin without DAC is not one");

// --- stopped: one by close(), the other at the end of the program
const closed = await pinsWhen(state => state[25].dac === false);
check(closed !== null && closed[25].voltage === 0 && closed[25].level === 0,
    "a closed output stops: the pin given back, at 0 V");
check(closed !== null && closed[26].dac === true, "... the other keeps driving");
const ended = await pinsWhen(state => state[26].dac === false);
check(ended !== null, "the outputs stop at the end of the program");
check(ended !== null && ended[26].voltage === 0, "... the pin back at 0 V");
check(await printed("dac end"), "the program ended after its outputs were read");

device.close();
report("dac");
