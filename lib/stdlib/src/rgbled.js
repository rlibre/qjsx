/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "stdlib/rgbled" of the standard library (lib/stdlib): strips of
// addressable RGB LEDs (WS2812 and the like), on top of the native module
// device/rmt, which sends their bits with exact timings.
//
//   import { RgbLed } from "stdlib/rgbled";
//   const led = new RgbLed(48, { count: 1, type: "ws2812" });
//   led.set(0, 255, 0, 0);       // LED 0 red (r, g, b: 0..255)
//   led.fill(0, 0, 32);          // every LED dim blue
//   led.setHsv(0, 120, 1, 0.25); // LED 0 green: hue in degrees, saturation and value 0..1
//   led.fillHsv(240, 1, 0.1);    // every LED dim blue
//   await led.show();            // sends the colors
//   led.close();
//
// A new model of LED is one more entry in TYPES: the order of its colors and
// the timings of its bits; the firmware does not change.
import { Rmt } from "device/rmt";

// The models of LED. Timings in ticks of 'resolution' (10 MHz: 0.1 us):
//   bit0, bit1  [high, low] of a 0 bit and of a 1 bit
//   reset       time low that latches the colors (newer WS2812B need > 280 us)
const TYPES = {
    ws2812: {
        order: "grb",
        resolution: 10_000_000,
        bit0: [4, 9],         // 0.4 us high, 0.9 us low
        bit1: [8, 5],         // 0.8 us high, 0.5 us low
        reset: 3000,          // 300 us
    },
};

/**
 * A color component as a byte: rounded, kept in 0..255.
 */
function component(value) {
    const number = Math.round(Number(value));
    if (!Number.isFinite(number)) {
        throw new TypeError("rgbled: a color component must be a number");
    }
    return Math.max(0, Math.min(255, number));
}

/**
 * [r, g, b] (0..255) of a color given as hue (degrees, any value: taken
 * modulo 360), saturation and value (0..1, kept in that range).
 */
function hsvToRgb(hue, saturation, value) {
    const h = ((Number(hue) % 360) + 360) % 360;
    const s = Math.max(0, Math.min(1, Number(saturation)));
    const v = Math.max(0, Math.min(1, Number(value)));
    if (!Number.isFinite(h) || Number.isNaN(s) || Number.isNaN(v)) {
        throw new TypeError("rgbled: hue, saturation and value must be numbers");
    }

    // the chroma, the second largest component, and what all three get
    const chroma = v * s;
    const second = chroma * (1 - Math.abs(((h / 60) % 2) - 1));
    const base = v - chroma;

    // the components in the sixth of the color wheel the hue falls in
    const sixths = [
        [chroma, second, 0],
        [second, chroma, 0],
        [0, chroma, second],
        [0, second, chroma],
        [second, 0, chroma],
        [chroma, 0, second],
    ];
    const [r, g, b] = sixths[Math.floor(h / 60) % 6];
    return [(r + base) * 255, (g + base) * 255, (b + base) * 255];
}

export class RgbLed {
    /**
     * A strip of 'count' LEDs of 'type' on 'pin', all off until show().
     */
    constructor(pin, { count = 1, type = "ws2812" } = {}) {
        const model = TYPES[type];
        if (model === undefined) {
            throw new RangeError(`rgbled: unknown type '${type}' (known: ${Object.keys(TYPES).join(", ")})`);
        }
        if (!Number.isInteger(count) || count < 1) {
            throw new RangeError("rgbled: count must be an integer >= 1");
        }

        this.count = count;
        this.model = model;
        this.bytes = new Uint8Array(count * 3);
        this.rmt = new Rmt(pin, { resolution: model.resolution });
    }

    /**
     * Sets the color of LED 'index' (sent by show()).
     */
    set(index, r, g, b) {
        if (!Number.isInteger(index) || index < 0 || index >= this.count) {
            throw new RangeError(`rgbled: index must be an integer from 0 to ${this.count - 1}`);
        }

        // the bytes of this LED, in the order of its model
        const color = { r: component(r), g: component(g), b: component(b) };
        const offset = index * 3;
        for (let i = 0; i < 3; i++) {
            this.bytes[offset + i] = color[this.model.order[i]];
        }
    }

    /**
     * Sets every LED to the same color (sent by show()).
     */
    fill(r, g, b) {
        for (let index = 0; index < this.count; index++) {
            this.set(index, r, g, b);
        }
    }

    /**
     * Sets the color of LED 'index' from hue (degrees), saturation and value
     * (0..1) (sent by show()).
     */
    setHsv(index, hue, saturation, value) {
        const [r, g, b] = hsvToRgb(hue, saturation, value);
        this.set(index, r, g, b);
    }

    /**
     * Sets every LED to the same color, as hue, saturation and value (sent by
     * show()).
     */
    fillHsv(hue, saturation, value) {
        const [r, g, b] = hsvToRgb(hue, saturation, value);
        this.fill(r, g, b);
    }

    /**
     * Sends the colors to the strip; a promise settled when they have been
     * sent (the LEDs then show them).
     */
    show() {
        const { bit0, bit1, reset } = this.model;
        return this.rmt.sendBytes(this.bytes, { bit0, bit1, reset });
    }

    /**
     * Frees the pin and its RMT channel (the LEDs keep their colors).
     */
    close() {
        this.rmt.close();
    }
}
