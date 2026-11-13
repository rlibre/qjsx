/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// device/pwm in the digital twin ("qjsx agent --board yd-esp32-s3"), whose
// board has the PWM of the ESP32-S3: 8 outputs, 4 timers, an 80 MHz clock,
// 14 bits. The program tests/fixtures/twinpwm prints what each call gives;
// this test checks its lines, then the pins as the page's server gives them
// (their PWM, their level once stopped).
//
//   qjsx run tests/pwm.test.js tcp:127.0.0.1:<agent port> <page port>
import * as fs from "sys/fs";
import { connect as netConnect } from "sys/net";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND } from "qjsx:protocol";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const PAGE_PORT = Number(process.argv[3]);
const PROJECT = "tests/fixtures/twinpwm";
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

// --- the agent: the twin offers device/pwm
const device = await Device.open(TARGET, { onLog });
await device.hello();
check(device.natives.includes("device/pwm"), "the twin offers device/pwm");

// --- the program
const projectBuild = build(PROJECT);
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(projectBuild.file));
await device.request(KIND.RUN);
check(await printed("pwm ready"), "the program opened its outputs");

// --- an output, its duty cycle and frequency, rounded as the board
equal(answer("default"), "1000 14 0", "by default 1 kHz on 14 bits, off");
equal(answer("quarter"), "0.25", "a quarter is a whole number of steps");
equal(answer("third"), "5461", "a third is rounded to the nearest step");
equal(answer("duty 1.5"), "RangeError", "a duty cycle above 1 is refused");
equal(answer("duty NaN"), "RangeError", "NaN is not a duty cycle");
equal(answer("20 kHz"), "20000 11 683", "20 kHz: 11 bits, the duty cycle kept on them");
equal(answer("3 kHz"), "2997.6 14", "3 kHz is rounded by the divider, as LEDC");

// --- refused
equal(answer("too low"), "EINVAL", "2 Hz is too low for the divider");
equal(answer("too high"), "RangeError", "above 40 MHz is refused by the module");
equal(answer("no such pin"), "EINVAL", "a pin the board does not have");
equal(answer("pin taken"), "EBUSY", "a pin that is an output already");

// --- the timers and outputs of the ESP32-S3
equal(answer("fifth frequency"), "EBUSY", "a fifth frequency at once: no timer left");
equal(answer("shared timer"), "100", "the same frequency shares a timer");
equal(answer("move"), "EBUSY 100", "a shared timer cannot leave when none is free: frequency kept");
equal(answer("ninth output"), "EBUSY", "a ninth output: none left");
equal(answer("closed"), "Error", "a closed output refuses its setters");
equal(answer("servo"), "50 1229", "a closed output frees its timer: 1.5 ms at 50 Hz");
equal(answer("pulse too long"), "RangeError", "a pulse longer than the period is refused");

// --- the pins on the page's server
const running = await pins();
equal(running[5].pwm?.frequency?.toFixed(1), "2997.6", "the page sees the frequency of an output");
equal(running[5].pwm?.duty, 5461 / 16384, "the page sees its duty cycle, rounded");
equal(running[5].mode, "output", "a PWM pin is an output");
equal(running[8].pwm?.invert, true, "the page sees an inverted output");
equal(running[6].level, 1, "a duty cycle of 1 holds the pin at 1");
equal(running[20].pwm, null, "a pin without output has no pwm");

// --- stopped: one by close(), the others at the end of the program
const closed = await pinsWhen(state => state[5].pwm === null);
check(closed !== null && closed[5].level === 0, "a closed output leaves its pin driven at 0");
const ended = await pinsWhen(state => state[8].pwm === null && state[6].pwm === null);
check(ended !== null, "the outputs stop at the end of the program");
check(ended !== null && ended[8].level === 1, "an inverted output stops at 1");
check(ended !== null && ended[6].level === 0 && ended[6].mode === "output", "the others stop driven at 0");

device.close();
report("pwm");
