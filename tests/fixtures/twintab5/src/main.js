/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The display built into the M5Stack Tab5 in its twin (tests/tab5.test.js):
// stdlib/display type "builtin" at rotation 0 (two rectangles, the whole
// screen), then at rotation 90 (one area); then its GT911 read with that
// display's rotation. Prints "shown 0", "shown 90", "touch ready <points>",
// each change of the touch ("touch <count> <x>,<y>"), "end" after 30 s.
import { Display } from "stdlib/display";
import { Touch } from "stdlib/touch";

// rotation 0: 720 x 1280, the whole screen
const upright = new Display({ type: "builtin", backlight: 22 });
upright.background = 0x000000;
upright.fillRect(10, 20, 30, 40, 0xff0000);
upright.fillRect(700, 1260, 20, 20, 0x00ff00);
await upright.show();
console.log(`shown 0 ${upright.width}x${upright.height}`);

// the test checks that image, then the next display takes the screen
await new Promise(resolve => setTimeout(resolve, 2000));
upright.close();

// rotation 90: 1280 x 720, one area of 50 x 10 at the top left
const wide = new Display({ type: "builtin", rotation: 90 });
wide.fillRect(0, 0, 50, 10, 0x0000ff);
await wide.show(0, 0, 50, 10);
console.log(`shown 90 ${wide.width}x${wide.height}`);

// the touch, in the coordinates of the wide display
const touch = new Touch({ type: "gt911", port: 0, sda: 31, scl: 32, interrupt: 23, display: wide });
touch.onChange(() => {
    const first = touch.points[0];
    console.log(`touch ${touch.count} ${touch.count > 0 ? `${first.x},${first.y}` : "-"}`);
});
console.log(`touch ready ${touch.maxPoints}`);

setTimeout(() => {
    touch.close();
    wide.close();
    console.log("end");
}, 30000);
