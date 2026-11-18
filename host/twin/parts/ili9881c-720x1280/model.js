/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The display built into a board: a 720 x 1280 panel driven by an ILI9881C
// on MIPI-DSI (the M5Stack Tab5's), refreshed from an image in memory. The
// twin does not play the DSI link nor the controller: the part makes the
// image (board.screen of qjsx:twin), sys/draw draws into it as on the board
// (stdlib/display, type "builtin"), and each area it makes visible is sent to
// the page. The backlight's brightness is the duty cycle of the PWM on its
// pin (or its level).
//
//   { "id": "screen", "model": "ili9881c-720x1280", "label": "Screen", "backlight": 22 }
//
// The page: every FLUSH_MS at most, the areas made visible since the last
// time, a few rectangles (DirtyRects of qjsx:twin: close areas merged,
// distant ones kept apart: one box around a header and a chart at the two
// ends of the panel was the whole image, 2.4 MB of base64 every 0.6 s, which
// starved the page), their pixels in base64 (16 bits, high byte first), with
// the look of the screen: { width, height,
// awake, on, inverted, brightness, turn (how the program's image is turned
// on the panel: 0, 90, 180, 270), rects: [{ x, y, width, height, pixels }] },
// as the part st7796-320x480 sends it (the same view shows both). Nothing is
// sent when nothing changed. The action "refresh" sends the whole image (a
// page that opens asks for it). Only the areas shown are read: no pixel is
// compared.
import { DirtyRects, Part, parsePin } from "qjsx:twin";

// The panel, in pixels, as it scans its image (the board held upright).
const WIDTH = 720;
const HEIGHT = 1280;

// How often the changes go to the page, in ms (15 times a second at most).
const FLUSH_MS = 66;

export default class Ili9881c extends Part {
    /**
     * The image of the display made (black), the backlight pin taken; then
     * the page told of the areas shown every FLUSH_MS.
     */
    constructor(config, board) {
        super(config, board);

        // the image, and the areas the program makes visible
        this._screen = board.screen(WIDTH, HEIGHT);
        this._screen.onFlush(area => this._onFlush(area));

        // the backlight pin: its PWM gives the brightness
        this._backlight = null;
        if (config.backlight !== undefined) {
            this._backlight = board.pin(parsePin(config.backlight, "backlight"));
        }

        // nothing shown yet
        this._dirty = new DirtyRects();   // the areas shown since the last flush
        this._turn = 0;                // the rotation of the last drawing
        this._lookSent = "";           // the look last sent, as JSON

        // the changes to the page, FLUSH_MS apart
        setInterval(() => this._flush(), FLUSH_MS);
    }

    /**
     * "refresh" from the page: the whole image is sent again.
     */
    onAction(action) {
        if (action === "refresh") {
            this._mark(0, 0, WIDTH - 1, HEIGHT - 1);
            this._lookSent = "";
        }
    }

    /**
     * An area made visible by the program: kept for the next flush, with
     * the rotation of its drawing.
     */
    _onFlush(area) {
        this._turn = area.rotation;
        this._mark(area.x, area.y, area.x + area.width - 1, area.y + area.height - 1);
    }

    /**
     * Adds a rectangle (its corners included) to the areas to send.
     */
    _mark(left, top, right, bottom) {
        this._dirty.add(left, top, right, bottom);
    }

    /**
     * The brightness of the backlight, 0 to 1: the duty cycle of its PWM,
     * else its level; 1 without a backlight pin.
     */
    _brightness() {
        // no backlight pin: always lit
        if (this._backlight === null) {
            return 1;
        }

        // a PWM: its active share (active low: the rest)
        const pwm = this._backlight.pwm;
        if (pwm !== null) {
            return pwm.invert ? 1 - pwm.duty : pwm.duty;
        }

        return this._backlight.level;
    }

    /**
     * Sends the page what changed: the pixels of the areas shown, and the
     * look of the screen; nothing when nothing changed.
     */
    _flush() {
        // the look now (the panel is always on: its backlight dims it)
        const look = {
            width: WIDTH,
            height: HEIGHT,
            awake: true,
            on: true,
            inverted: false,
            brightness: Math.round(this._brightness() * 1000) / 1000,
            turn: this._turn,
        };
        const lookText = JSON.stringify(look);

        // nothing changed: nothing sent
        const rects = this._dirty.take();
        if (rects.length === 0 && lookText === this._lookSent) {
            return;
        }

        // the look, and the pixels of the areas
        this._lookSent = lookText;
        this.publish({ ...look, rects: rects.map(rect => this._pixelsOf(rect)) });
    }

    /**
     * The pixels of a rectangle of the image, row by row, 16 bits each, high
     * byte first, in base64.
     */
    _pixelsOf({ left, top, right, bottom }) {
        const width = right - left + 1;
        const height = bottom - top + 1;
        const bytes = this._screen.pixels(left, top, width, height);
        return { x: left, y: top, width, height, pixels: bytes.toBase64() };
    }
}
