/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A program for stdlib/display in the digital twin of the WT32-SC01
// (tests/display.test.js): the screen of the board, an ST7796 of 320 x 480
// on SPI2. Each step draws, shows, prints "shown <step>", then waits for
// the test to force GPIO4 to the next level (1, 0, 1...) before the next
// one, so that the test reads the screen between two steps.
//   1. rotation 0: rectangles, a line, a text, the whole screen;
//   2. white everywhere, but only the area (10, 20, 20, 10) shown;
//   3. rotation 90, then 180, then 270: a red rectangle of 10 x 5 at (0, 0);
//   4. the backlight at half;
//   5. rotation 90, a value in a proportional font shown 20 times.
import { Gpio } from "device/gpio";
import { Display } from "stdlib/display";
import inter16 from "./inter16.bin" with { type: "bytes" };

// The display of the board, turned by 'rotation'.
function open(rotation) {
    return new Display({ type: "st7796", port: 2, sclk: 14, mosi: 13, cs: 15, dc: 21, reset: 22, backlight: 23, rotation });
}

// The test's go: GPIO4, pulled down, forced by the test from the page.
const go = new Gpio(4, { mode: "input", pull: "down" });
let level = 0;

// Waits for the test: GPIO4 at the next level.
async function next() {
    level = 1 - level;
    await go.waitFor(level, { timeout: 10000 });
}

// 1. rotation 0, the whole screen
let lcd = open(0);
const font = lcd.font(inter16);
lcd.fillRect(10, 20, 30, 40, 0xff0000);
lcd.fillRect(100, 200, 5, 5, 0x00ff00);
lcd.line(0, 479, 319, 479, 0x0000ff);
lcd.text(50, 300, "Hi", font, 0xffffff);
await lcd.show();
console.log(`text ${lcd.textWidth("Hi", font)} ${font.lineHeight}`);
console.log("shown 1");

// 2. white everywhere, one area shown
await next();
lcd.fillRect(0, 0, lcd.width, lcd.height, 0xffffff);
await lcd.show(10, 20, 20, 10);
console.log("shown 2");

// 3. the three other rotations, a red rectangle at (0, 0) each
for (const rotation of [90, 180, 270]) {
    await next();
    lcd.close();
    lcd = open(rotation);
    lcd.fillRect(0, 0, 10, 5, 0xff0000);
    await lcd.show(0, 0, 10, 5);
    console.log(`shown ${rotation} ${lcd.width}x${lcd.height}`);
}

// 4. the backlight at half
await next();
lcd.backlight(0.5);
console.log("shown backlight");

// 5. rotation 90 again, values that change as in a demo: a panel and a
// line of text in a proportional font shown again and again, 20 times
await next();
lcd.close();
lcd = open(90);
const valueFont = lcd.font(inter16);
for (let step = 0; step < 20; step++) {
    const text = `${(20 + 15 * Math.sin(step / 3)).toFixed(1)} °C`;
    lcd.fillRect(40, 120, 400, 80, 0x000000);
    lcd.text(60, 150, text, valueFont, 0xffd040);
    await lcd.show(40, 120, 400, 80);
    lcd.fillRect(0, 266, 480, 54, 0x000000);
    lcd.text(250, 280, text, valueFont, 0x60c0ff);
    await lcd.show(0, 266, 480, 54);
    await new Promise(resolve => setTimeout(resolve, 30));
}
console.log("shown values");

// the end: the test reads the last state, then the program ends
await next();
lcd.close();
go.close();
console.log("display end");
