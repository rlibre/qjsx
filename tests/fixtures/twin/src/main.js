/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A program for the digital twin of the YD-ESP32-S3 (tests/twin.test.js):
// the board's platform, the BOOT button (GPIO0, pulled up by the board), an
// output, the RGB LED (GPIO48). It waits for BOOT, which the test presses
// through the page's WebSocket.
import * as fs from "sys/fs";
import { Gpio } from "device/gpio";
import { RgbLed } from "stdlib/rgbled";

console.log(`platform ${process.platform}`);

// the twin reaches only its data folder, as the board its partition (the
// target is the agent's own storage folder)
try {
    fs.writeFileSync("../escaped.txt", "escaped");
    console.log("escape: ok");
} catch (error) {
    console.log(`escape: ${error.code}`);
}

// BOOT at rest: 1, the board pulls it up
const boot = new Gpio(0, { mode: "input" });
console.log(`boot at rest ${boot.read()}`);

// an output at 1
const output = new Gpio(2, { mode: "output" });
output.write(1);
console.log(`output ${output.read()}`);

// the LED, one color
const led = new RgbLed(48);
led.set(0, 10, 20, 30);
await led.show();
console.log("led shown");

// BOOT pressed by the test
console.log("waiting for boot");
await boot.waitFor(0, { timeout: 5000 });
console.log("boot pressed");

boot.close();
output.close();
led.close();
