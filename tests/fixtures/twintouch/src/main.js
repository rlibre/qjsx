/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// stdlib/touch in the twin of the WT32-SC01 (tests/touch.test.js): its
// FT6336 ("touch", interrupt GPIO39) read at rotation 0, the same FT6336
// read by a second touch with its mounting undone (swapXY, mirrorX; polled),
// and a GT911 of the project ("gt", range 640 x 960, interrupt GPIO27) read
// at rotation 90. Every change printed: "<name> <count> <x>,<y>" ("-" when
// no finger), then "touch end" after 60 s.
import { I2c } from "device/i2c";
import { Touch } from "stdlib/touch";

const bus = new I2c(0, { sda: 18, scl: 19, frequency: 400000 });

// prints each change of 'touch' as "<name> <count> <x>,<y>"
function follow(name, touch) {
    touch.onChange(() => {
        const first = touch.points[0];
        const where = touch.count > 0 ? `${first.x},${first.y}` : "-";
        console.log(`${name} ${touch.count} ${where}`);
    });
}

const ft = new Touch({ type: "ft6336", bus, interrupt: 39, width: 320, height: 480 });
const mounted = new Touch({ type: "ft6336", bus, width: 320, height: 480, swapXY: true, mirrorX: true });
const gt = new Touch({ type: "gt911", bus, interrupt: 27, width: 480, height: 320, rotation: 90 });
follow("ft", ft);
follow("mounted", mounted);
follow("gt", gt);
console.log(`ready ${ft.maxPoints} ${gt.maxPoints}`);

// alive for the test, then closed
setTimeout(() => {
    ft.close();
    mounted.close();
    gt.close();
    bus.close();
    console.log("touch end");
}, 60000);
