/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "stdlib/display" of the standard library (lib/stdlib): color
// displays driven over SPI (a controller with a data / command pin), on top
// of the native modules device/spi, device/gpio (reset), device/pwm
// (backlight) and sys/draw (the rendering).
//
//   import { Display } from "stdlib/display";
//   const lcd = new Display({
//       type: "st7796", port: 2,
//       sclk: 14, mosi: 13, cs: 15, dc: 21, reset: 22, backlight: 23,
//       rotation: 90,
//   });
//   lcd.fillRect(10, 10, 100, 40, 0xff0000);   // colors: 0xRRGGBB
//   lcd.line(0, 0, lcd.width - 1, lcd.height - 1, 0xffffff);
//   lcd.arc(240, 200, 80, -135, 135, 12, 0x30c060);   // degrees, 0 at the top, clockwise
//   lcd.fillRoundRect(20, 20, 200, 60, 12, 0x203050);    // corners of radius 12
//   lcd.roundRect(20, 20, 200, 60, 12, 2, 0xffffff);     // its outline, 2 pixels
//   import inter16 from "./fonts/inter16.bin" with { type: "bytes" };
//   const body = lcd.font(inter16);            // its file stays in flash
//   lcd.text(10, 60, "Température", body, 0xffffff);
//   const value = lcd.font(inter16, { tabularDigits: true });
//   lcd.text(10, 90, "21.5 °C", value, 0xffff00);
//   await lcd.show();                          // the image goes to the screen
//   lcd.backlight(0.5);
//   lcd.close();
//
// The drawing calls draw nothing: each one adds a command to a list (a few
// bytes; no pixels are kept, there is no copy of the screen). show() hands
// the list to the task of the SPI bus, which renders the image a few rows
// at a time while it sends them: the program goes on meanwhile, and starts
// the next image at once in the list, empty again.
//
// What follows from it:
//   - an image is made of everything drawn since the last show(): the
//     display does not remember the image before. What no command covers in
//     the area shown takes the 'background' color;
//   - show(x, y, width, height) sends an area only: the commands are drawn
//     again there, the rest of the screen stays;
//   - a show() called while an image is still going out is ignored (its
//     promise resolves with false) and the list is kept;
//   - a command added to a full list is dropped, counted in 'dropped';
//   - fonts are files in LVGL's binary format, made without compression by
//     lv_font_conv (tools/fonts/), imported as bytes: read in place, in
//     flash on a device. A text is one line; a character its font lacks is
//     skipped.
//
// The idea of a display list rendered a few rows at a time comes from Poco,
// the renderer of the Moddable SDK (https://github.com/Moddable-OpenSource/moddable).
// No code of it is used: this is our own implementation.
//
// A new controller is one more entry in TYPES: its size, its commands.
import { Gpio } from "device/gpio";
import { Pwm } from "device/pwm";
import { Spi } from "device/spi";
import { fontMetrics, show, textWidth } from "sys/draw";

// The commands every such controller has (MIPI DCS).
const SLEEP_OUT = 0x11;
const DISPLAY_ON = 0x29;
const COLUMN_RANGE = 0x2a;
const ROW_RANGE = 0x2b;
const MEMORY_WRITE = 0x2c;
const MEMORY_ACCESS = 0x36;
const PIXEL_FORMAT = 0x3a;

// 16 bits per pixel (5 red, 6 green, 5 blue), the format of the pixels sent.
const PIXEL_FORMAT_16_BITS = 0x55;

// How long the controller needs after its reset pin and after SLEEP_OUT, ms.
const RESET_PULSE_MS = 10;
const AFTER_RESET_MS = 120;
const AFTER_SLEEP_OUT_MS = 120;

// The PWM of the backlight, Hz.
const BACKLIGHT_HZ = 5000;

// A command of the list, as sys/draw reads it: 6 items of an Int16Array,
// [kind, pixel, x0, y0, x1, y1]; a text is followed by its characters.
const COMMAND_ITEMS = 6;
const FILL_RECT = 0;                // x0, y0: first pixel; x1, y1: width and height
const LINE = 1;                     // from (x0, y0) to (x1, y1), both included
const TEXT = 2;                    // top left (x0, y0); x1: font index | flags << 4; y1: length
const ARC = 3;                     // center (x0, y0); x1: outer radius; y1: thickness; then start, end
const ROUND_RECT = 4;              // first pixel (x0, y0); x1, y1: width, height; then radius, thickness
const ARC_EXTRA_ITEMS = 2;          // the angles of an arc, in tenths of a degree
const ROUND_RECT_EXTRA_ITEMS = 2;   // the radius and thickness of a rounded rectangle (0: filled)

// Flags of a text (bits 4 and above of its x1), and the most fonts of a
// display (its font index is 4 bits).
const TABULAR_DIGITS = 1;           // every digit as wide as the widest one
const MAX_FONTS = 16;

// The largest transfer of an SPI bus, bytes (device/spi): a band of rows
// rendered and sent at once must fit in it.
const MAX_TRANSFER = 32768;

// The controllers.
//   width, height   pixels at rotation 0
//   frequency       SPI clock, Hz
//   rotations       value of MEMORY_ACCESS for the image turned clockwise by
//                   0, 90, 180 and 270 degrees
const TYPES = {
    st7796: {
        width: 320,
        height: 480,
        frequency: 40_000_000,
        rotations: [0x48, 0x28, 0x88, 0xe8],
    },
};

const ROTATIONS = [0, 90, 180, 270];

/**
 * Blocks the program for 'ms' milliseconds: the controller's set-up needs
 * waits, and the constructor cannot await.
 */
function waitMs(ms) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
        // nothing: only the time passes
    }
}

/**
 * The 16 bits of a pixel (5 of red, 6 of green, 5 of blue) of a color
 * 0xRRGGBB.
 */
function pixelOf(color) {
    return ((color >> 8) & 0xf800) | ((color >> 5) & 0x07e0) | ((color >> 3) & 0x001f);
}

export class Display {
    #bus;                           // the SPI bus, owned by the display
    #device;                        // the controller on it
    #reset = null;                  // Gpio of the reset pin; null: none
    #backlight = null;              // Pwm of the backlight pin; null: none
    #command = new Uint8Array(1);   // the command sent, written in place
    #range = new Uint8Array(4);     // the range sent: first and last, high byte first
    #items;                         // the list: COMMAND_ITEMS items per command, a text followed by its characters
    #used = 0;                      // items in the list
    #fonts = [];                    // the font files the texts use, by index (kept while the display lives)
    #bandRows;                      // rows rendered and sent at a time
    #isShowing = false;             // an image is going out

    /**
     * Opens the display: resets the controller, sets it up, turns the
     * screen and the backlight on. Takes about a quarter of a second.
     * 'commands': room of the list, in commands of 6 items (a text takes
     * one more item per character); 'bandRows': rows rendered and
     * sent at a time (two buffers of that many rows are kept while showing:
     * one is made while the other goes out).
     */
    constructor({ type = "st7796", port, sclk, mosi, cs, dc, reset, backlight, frequency, rotation = 0, commands = 256, bandRows = 16 } = {}) {
        // the controller, and which way the image is turned
        const model = TYPES[type];
        if (model === undefined) {
            throw new RangeError(`display: unknown type '${type}' (known: ${Object.keys(TYPES).join(", ")})`);
        }

        const turn = ROTATIONS.indexOf(rotation);
        if (turn < 0) {
            throw new RangeError("display: rotation must be 0, 90, 180 or 270");
        }

        // the pin that tells commands from pixels: no display without it
        if (dc === undefined) {
            throw new TypeError("display: the dc pin is required");
        }

        // its size as the program sees it: exchanged when on its side
        const isOnItsSide = turn % 2 === 1;
        this.type = type;
        this.rotation = rotation;
        this.width = isOnItsSide ? model.height : model.width;
        this.height = isOnItsSide ? model.width : model.height;
        this.background = 0x000000;         // color of what no command covers
        this.dropped = 0;                   // commands refused since the last show(): the list was full

        // the list, and bands that fit in one transfer of the bus
        const rowBytes = this.width * 2;
        const mostRows = Math.floor(MAX_TRANSFER / rowBytes);
        this.#items = new Int16Array(commands * COMMAND_ITEMS);
        this.#bandRows = Math.max(1, Math.min(bandRows, mostRows));

        // the bus, for transfers of one band (device/spi checks the pins)
        this.#bus = new Spi(port, { sclk, mosi, maxTransfer: this.#bandRows * rowBytes });

        // the controller set up; a step that fails gives back what was
        // opened (the bus, the pins), so that the program may try again
        try {
            this.#start(model, turn, { cs, dc, frequency, reset, backlight });
        } catch (error) {
            this.close();
            throw error;
        }
    }

    /**
     * Opens the controller on the bus, resets it, sets it up, turns the
     * screen and the backlight on (the end of the constructor).
     */
    #start(model, turn, { cs, dc, frequency, reset, backlight }) {
        // the controller on the bus
        this.#device = this.#bus.device({ cs, dc, frequency: frequency ?? model.frequency });

        // a pulse on the reset pin, when there is one
        if (reset !== undefined) {
            this.#reset = new Gpio(reset, { mode: "output" });
            this.#reset.write(0);
            waitMs(RESET_PULSE_MS);
            this.#reset.write(1);
            waitMs(AFTER_RESET_MS);
        }

        // out of sleep, 16 bits per pixel, the image turned, the screen on
        this.#send(SLEEP_OUT);
        waitMs(AFTER_SLEEP_OUT_MS);
        this.#send(PIXEL_FORMAT);
        this.#device.writeSync([PIXEL_FORMAT_16_BITS]);
        this.#send(MEMORY_ACCESS);
        this.#device.writeSync([model.rotations[turn]]);
        this.#send(DISPLAY_ON);

        // the backlight fully on, when there is one
        if (backlight !== undefined) {
            this.#backlight = new Pwm(backlight, { frequency: BACKLIGHT_HZ, duty: 1 });
        }
    }

    /**
     * Adds a filled rectangle to the image.
     */
    fillRect(x, y, width, height, color) {
        this.#add(FILL_RECT, x, y, width, height, color);
    }

    /**
     * Adds a line from (x0, y0) to (x1, y1), both included, one pixel
     * thick.
     */
    line(x0, y0, x1, y1, color) {
        this.#add(LINE, x0, y0, x1, y1, color);
    }

    /**
     * Adds one pixel.
     */
    pixel(x, y, color) {
        this.#add(FILL_RECT, x, y, 1, 1, color);
    }

    /**
     * Adds an arc of a ring centered on (cx, cy): from 'radius' (its outer
     * edge) inward over 'thickness' pixels, from angle 'start' to 'end' in
     * degrees, 0 at the top, clockwise (an end at or before the start:
     * nothing; a whole turn or more: the whole ring). Its edges are
     * smoothed; its ends are cut along the radius.
     */
    arc(cx, cy, radius, start, end, thickness, color) {
        // room for the command and its two angles
        const items = this.#items;
        const at = this.#used;
        if (at + COMMAND_ITEMS + ARC_EXTRA_ITEMS > items.length) {
            this.dropped++;
            return;
        }

        // the command, then the angles in tenths of a degree
        items[at] = ARC;
        items[at + 1] = pixelOf(color);
        items[at + 2] = cx;
        items[at + 3] = cy;
        items[at + 4] = radius;
        items[at + 5] = thickness;
        items[at + 6] = Math.round(start * 10);
        items[at + 7] = Math.round(end * 10);
        this.#used = at + COMMAND_ITEMS + ARC_EXTRA_ITEMS;
    }

    /**
     * Adds a filled rectangle with rounded corners: the same 'radius' at
     * the four corners (at most half the smaller side). The corners are
     * smoothed.
     */
    fillRoundRect(x, y, width, height, radius, color) {
        this.#addRoundRect(x, y, width, height, radius, 0, color);
    }

    /**
     * Adds the outline of a rectangle with rounded corners, 'thickness'
     * pixels wide inside its box.
     */
    roundRect(x, y, width, height, radius, thickness, color) {
        this.#addRoundRect(x, y, width, height, radius, thickness, color);
    }

    /**
     * Adds a circle: a whole ring of 'thickness' pixels inside 'radius'.
     */
    circle(cx, cy, radius, thickness, color) {
        this.arc(cx, cy, radius, 0, 360, thickness, color);
    }

    /**
     * Adds a filled disc of 'radius'.
     */
    disc(cx, cy, radius, color) {
        this.arc(cx, cy, radius, 0, 360, radius + 1, color);
    }

    /**
     * Adds the outline of a rectangle, one pixel thick.
     */
    rect(x, y, width, height, color) {
        // top and bottom, then left and right between them
        this.#add(FILL_RECT, x, y, width, 1, color);
        this.#add(FILL_RECT, x, y + height - 1, width, 1, color);
        this.#add(FILL_RECT, x, y + 1, 1, height - 2, color);
        this.#add(FILL_RECT, x + width - 1, y + 1, 1, height - 2, color);
    }

    /**
     * A font for text(): 'bytes' is a font file (LVGL's binary format,
     * uncompressed), imported with { type: "bytes" }. With 'tabularDigits'
     * the digits all take the width of the widest one: a value that changes
     * does not move what follows it. Returns what text() takes, with the
     * font's size, ascent, descent and lineHeight in pixels. Throws a
     * TypeError when the file is not such a font.
     */
    font(bytes, { tabularDigits = false } = {}) {
        // read once (TypeError when not a font)
        const metrics = fontMetrics(bytes);

        // the same file is given once to the renderer
        let index = this.#fonts.indexOf(bytes);
        if (index < 0) {
            if (this.#fonts.length >= MAX_FONTS) {
                throw new RangeError(`display: at most ${MAX_FONTS} fonts`);
            }
            index = this.#fonts.push(bytes) - 1;
        }

        const flags = tabularDigits ? TABULAR_DIGITS : 0;
        return { index, flags, bytes, ...metrics };
    }

    /**
     * Adds a line of text, the top left of its line at (x, y), in a font
     * made by font(). Its baseline is font.ascent below y.
     */
    text(x, y, string, font, color) {
        // as a string (a number is written as it would print); room for the
        // command and its characters
        const characters = String(string);
        const items = this.#items;
        const at = this.#used;
        const length = characters.length;
        if (at + COMMAND_ITEMS + length > items.length) {
            this.dropped++;
            return;
        }

        // the command, then each UTF-16 unit
        items[at] = TEXT;
        items[at + 1] = pixelOf(color);
        items[at + 2] = x;
        items[at + 3] = y;
        items[at + 4] = font.index | (font.flags << 4);
        items[at + 5] = length;

        const first = at + COMMAND_ITEMS;
        for (let index = 0; index < length; index++) {
            items[first + index] = characters.charCodeAt(index);
        }

        this.#used = first + length;
    }

    /**
     * The width in pixels of 'string' drawn by text() in 'font'.
     */
    textWidth(string, font) {
        return textWidth(font.bytes, string, font.flags);
    }

    /**
     * Empties the image being made, without showing it.
     */
    clear() {
        this.#used = 0;
        this.dropped = 0;
    }

    /**
     * Shows the image on the screen, or on one area of it (cut to the
     * screen), and starts the next one, empty. A promise resolved with true
     * once the image is on the screen; with false at once, the image kept,
     * when another one is still going out.
     */
    show(x = 0, y = 0, width = this.width, height = this.height) {
        // one image at a time
        if (this.#isShowing) {
            return Promise.resolve(false);
        }

        // never throws: a failure (closed, a bad list) rejects, and the
        // display is free for the next image
        try {
            return this.#startShow(x, y, width, height);
        } catch (error) {
            this.#isShowing = false;
            return Promise.reject(error);
        }
    }

    /**
     * Sends the area to the controller and hands the list to sys/draw: the
     * work of show(), which may throw here.
     */
    #startShow(x, y, width, height) {
        // the area, on the screen; none: the image is dropped
        const left = Math.max(0, x | 0);
        const top = Math.max(0, y | 0);
        const right = Math.min(this.width, (x + width) | 0);
        const bottom = Math.min(this.height, (y + height) | 0);
        const count = this.#used;
        this.clear();
        if (right <= left || bottom <= top) {
            return Promise.resolve(true);
        }

        // the controller told where the pixels go
        this.#sendRange(COLUMN_RANGE, left, right - 1);
        this.#sendRange(ROW_RANGE, top, bottom - 1);
        this.#send(MEMORY_WRITE);

        // the pixels: rendered from a copy of the list while they are sent
        this.#isShowing = true;
        const sent = show(this.#device, this.#items, count, left, top, right - left, bottom - top, pixelOf(this.background), this.#bandRows, this.#fonts);
        return sent.then(
            () => {
                this.#isShowing = false;
                return true;
            },
            error => {
                this.#isShowing = false;
                throw error;
            },
        );
    }

    /**
     * Sets the brightness of the backlight: 0 (off) to 1 (full).
     */
    backlight(level) {
        if (this.#backlight === null) {
            throw new Error("display: no backlight pin was given");
        }
        this.#backlight.setDuty(level);
    }

    /**
     * Frees the pins: the bus, the reset pin, the backlight (which goes
     * off). The screen keeps its image.
     */
    close() {
        this.#backlight?.close();
        this.#reset?.close();
        this.#bus.close();
    }

    /**
     * Adds a command to the list: written in place, no pixel drawn. A full
     * list drops it.
     */
    #add(kind, x0, y0, x1, y1, color) {
        // full
        const items = this.#items;
        const at = this.#used;
        if (at + COMMAND_ITEMS > items.length) {
            this.dropped++;
            return;
        }

        items[at] = kind;
        items[at + 1] = pixelOf(color);
        items[at + 2] = x0;
        items[at + 3] = y0;
        items[at + 4] = x1;
        items[at + 5] = y1;
        this.#used = at + COMMAND_ITEMS;
    }

    /**
     * Adds a rounded rectangle to the list ('thickness' 0: filled).
     */
    #addRoundRect(x, y, width, height, radius, thickness, color) {
        // room for the command, its radius and thickness
        const items = this.#items;
        const at = this.#used;
        if (at + COMMAND_ITEMS + ROUND_RECT_EXTRA_ITEMS > items.length) {
            this.dropped++;
            return;
        }

        items[at] = ROUND_RECT;
        items[at + 1] = pixelOf(color);
        items[at + 2] = x;
        items[at + 3] = y;
        items[at + 4] = width;
        items[at + 5] = height;
        items[at + 6] = radius;
        items[at + 7] = thickness;
        this.#used = at + COMMAND_ITEMS + ROUND_RECT_EXTRA_ITEMS;
    }

    /**
     * Sends one command (no parameter).
     */
    #send(command) {
        this.#command[0] = command;
        this.#device.commandSync(this.#command);
    }

    /**
     * Sends the range 'first' to 'last' (both included) of 'command'
     * (COLUMN_RANGE or ROW_RANGE).
     */
    #sendRange(command, first, last) {
        const range = this.#range;
        range[0] = first >> 8;
        range[1] = first;
        range[2] = last >> 8;
        range[3] = last;
        this.#send(command);
        this.#device.writeSync(range);
    }
}
