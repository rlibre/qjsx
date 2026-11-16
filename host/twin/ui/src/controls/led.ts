/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// An LED of the board: a disc lit with a color (off: dark).
import { Component, ComponentProps } from "x4js";

// How bright the faintest lit LED is drawn, 0 to 1.
const FAINTEST = 0.35;

// The light of the three LEDs of a WS2812 (about 625, 520 and 470 nm) as the
// linear light of a screen (sRGB), from the CIE 1931 observer; each column
// (one LED) is scaled so that equal values give the white of the screen: each
// row adds up to 1. A row is a channel of the screen, its three numbers the
// shares of the red, green and blue LEDs.
// The green LED is purer than a screen's green and leans toward blue: a green
// with a little blue looks nearly cyan, while [64, 64, 64] stays white (seen
// on the client board, user 2026-11-16; replaces a gain of 2 on the blue,
// which turned white into light blue).
const LED_TO_SCREEN = [
    [1.914, -0.796, -0.118],                // red of the screen
    [-0.124, 1.096, 0.028],                 // green of the screen
    [-0.023, -0.050, 1.073],                // blue of the screen
];

/**
 * An sRGB value (0 to 1) of a linear light (0 to 1).
 */
function linearToSrgb(light: number) {
    return light <= 0.0031308 ? 12.92 * light : 1.055 * Math.pow(light, 1 / 2.4) - 0.055;
}

/**
 * The CSS color of a lit LED as the eye sees it (a WS2812 at 24/255 already
 * looks plainly red). The values sent to the LED are light (PWM), not sRGB:
 * 1. each LED as the eye sees it: the cube root of its light (the eye
 *    compresses a point of light strongly), each LED on its own, so a weak
 *    one counts more: the little blue of a dim sea green makes it nearly cyan
 *    (seen on the client board, user 2026-11-16), equal values stay white;
 * 2. the same on the screen (LED_TO_SCREEN);
 * 3. a color purer than the screen can show (a channel below 0): white added
 *    until it can, which keeps its hue;
 * 4. the hue: each channel's share of the strongest, as sRGB;
 * 5. how bright the LED looks: the cube root of its strongest LED, from
 *    FAINTEST.
 * At least one channel must be above 0.
 */
function visibleColor(red: number, green: number, blue: number) {
    // each LED as the eye sees it
    const [seenRed, seenGreen, seenBlue] = [red, green, blue].map(value => Math.cbrt(value / 255));

    // the same on the screen
    const screen = LED_TO_SCREEN.map(row => row[0] * seenRed + row[1] * seenGreen + row[2] * seenBlue);

    // into the colors the screen can show
    const lowest = Math.min(...screen);
    const shown = lowest < 0 ? screen.map(value => value - lowest) : screen;
    const strongest = Math.max(...shown);

    // how bright it looks, 0 to 1
    const strongestLed = Math.max(red, green, blue);
    const brightness = FAINTEST + (1 - FAINTEST) * Math.cbrt(Math.min(1, strongestLed / 255));

    // each channel: its share of the strongest, as sRGB, at that brightness
    const [r, g, b] = shown.map(value => Math.round(linearToSrgb(value / strongest) * 255 * brightness));
    return `rgb(${r}, ${g}, ${b})`;
}

export class Led extends Component<ComponentProps> {
    static classPrefix = "twin";

    /**
     * Lights the LED with a color, each channel 0 to 255 (all 0: off). The
     * color is the CSS variable --led-color: the lens, its glow (main.scss).
     */
    setColor(red: number, green: number, blue: number) {
        const off = red === 0 && green === 0 && blue === 0;
        this.setClass("off", off);
        this.setStyleVariable("--led-color", off ? "" : visibleColor(red, green, blue));
    }
}
