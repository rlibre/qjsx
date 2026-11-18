/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "stdlib/touch" of the standard library (lib/stdlib): the touch
// controllers of displays, on I2C, on top of the native modules device/i2c
// and device/gpio (the interrupt pin).
//
//   import { Touch } from "stdlib/touch";
//   const touch = new Touch({ type: "gt911", port: 0, sda: 31, scl: 32, interrupt: 23, display: lcd });
//   touch.onChange(() => {
//       for (let i = 0; i < touch.count; i++) {
//           const point = touch.points[i];     // { id, x, y }, in the coordinates of the drawing
//           lcd.disc(point.x, point.y, 8, 0xff0000);
//       }
//   });
//   touch.close();
//
// How it reads:
//   - with an interrupt pin, nothing is read while nobody touches: the
//     controller's signal starts the reads. Without one, the controller is
//     read 'rate' times a second, always;
//   - while a finger is on the screen, it is read 'rate' times a second at
//     most (33 by default): the reports the controller makes in between
//     (about 100 a second) are not read, the next read takes the position
//     of the moment;
//   - onChange() is called when a finger comes or goes, or moves by
//     'threshold' pixels or more (2 by default: a finger held still trembles
//     by a pixel or two);
//   - 'points' is made once: maxPoints objects updated in place, nothing is
//     made at each move. A program that keeps a point copies its x and y.
//
// A controller is one entry in TYPES: its address, how to read its
// configuration and its reports.
import { Gpio } from "device/gpio";
import { I2c } from "device/i2c";

// What a reading touch does by default.
const DEFAULT_RATE = 33;             // reads a second at most
const DEFAULT_THRESHOLD = 2;         // pixels: a smaller move is not reported
const BUS_FREQUENCY = 400000;        // Hz, the clock of a bus the touch opens itself

const ROTATIONS = [0, 90, 180, 270];

// One point of a report, in the scratch of the reads: its id, x, y.
const POINT_ITEMS = 3;

// GT911 (Goodix): 16-bit register addresses, high byte first. The
// configuration gives the range of the coordinates and the most points; a
// report is a status byte (bit 7: a new report, bits 0-3: its points) then 8
// bytes per point (id, x, y little-endian, size); the status is cleared by
// writing 0, else the controller makes no new report.
const GT911_CONFIG = [0x80, 0x48];          // x range (2 bytes), y range (2 bytes), most points
const GT911_CONFIG_BYTES = 5;
const GT911_STATUS = [0x81, 0x4e];
const GT911_CLEAR = [0x81, 0x4e, 0x00];
const GT911_NEW_REPORT = 0x80;
const GT911_POINT_BYTES = 8;
const GT911_MOST_POINTS = 10;

// FT6336 (FocalTech): 8-bit register addresses. Register 2 holds the number
// of points, then 6 bytes per point (event and x high bits, x low, id and y
// high bits, y low, weight, area). Always readable: no new report flag.
const FT6336_POINTS = [0x02];
const FT6336_POINT_BYTES = 6;
const FT6336_MOST_POINTS = 2;

// The controllers.
//   address        its I2C address
//   open(device)   reads what it is set to: { maxPoints, rangeX, rangeY }
//                  (range 0: the coordinates are already pixels of the panel)
//   request        the bytes written before a report is read
//   reportBytes(n) the bytes of a report of n points at most
//   parse(bytes, scratch, maxPoints)
//                  writes id, x, y of each point into scratch; returns how
//                  many, or -1 when the controller has no new report
//   clear          the bytes written once a report is read, or null
const TYPES = {
    gt911: {
        address: 0x14,
        open(device) {
            const config = device.writeReadSync(GT911_CONFIG, GT911_CONFIG_BYTES);
            const mostPoints = config[4] & 0x0f;
            return {
                maxPoints: Math.max(1, Math.min(mostPoints, GT911_MOST_POINTS)),
                rangeX: config[0] | (config[1] << 8),
                rangeY: config[2] | (config[3] << 8),
            };
        },
        request: GT911_STATUS,
        reportBytes(points) {
            return 1 + GT911_POINT_BYTES * points;
        },
        parse(bytes, scratch, maxPoints) {
            // no new report: nothing changed
            const status = bytes[0];
            if ((status & GT911_NEW_REPORT) === 0) {
                return -1;
            }

            // each point: id, x, y (little-endian)
            const count = Math.min(status & 0x0f, maxPoints);
            for (let i = 0; i < count; i++) {
                const at = 1 + i * GT911_POINT_BYTES;
                scratch[i * POINT_ITEMS] = bytes[at];
                scratch[i * POINT_ITEMS + 1] = bytes[at + 1] | (bytes[at + 2] << 8);
                scratch[i * POINT_ITEMS + 2] = bytes[at + 3] | (bytes[at + 4] << 8);
            }
            return count;
        },
        clear: GT911_CLEAR,
    },
    ft6336: {
        address: 0x38,
        open() {
            return { maxPoints: FT6336_MOST_POINTS, rangeX: 0, rangeY: 0 };
        },
        request: FT6336_POINTS,
        reportBytes(points) {
            return 1 + FT6336_POINT_BYTES * points;
        },
        parse(bytes, scratch, maxPoints) {
            // the number of points (0x0f, or more than it can see: none)
            const told = bytes[0] & 0x0f;
            const count = told <= maxPoints ? told : 0;

            // each point: x and y on 12 bits, the id in the high bits of y's
            for (let i = 0; i < count; i++) {
                const at = 1 + i * FT6336_POINT_BYTES;
                scratch[i * POINT_ITEMS] = bytes[at + 2] >> 4;
                scratch[i * POINT_ITEMS + 1] = ((bytes[at] & 0x0f) << 8) | bytes[at + 1];
                scratch[i * POINT_ITEMS + 2] = ((bytes[at + 2] & 0x0f) << 8) | bytes[at + 3];
            }
            return count;
        },
        clear: null,
    },
};

export class Touch {
    #model;                         // the entry of TYPES
    #bus;                           // the I2C bus
    #ownsBus;                       // true: opened here, closed by close()
    #device;                        // the controller on the bus
    #interrupt = null;              // Gpio of the interrupt pin; null: polled
    #period;                        // ms between two reads, at least
    #threshold;                     // pixels: a smaller move is not reported
    #panelWidth;                    // the panel at rotation 0, pixels
    #panelHeight;
    #rangeX;                        // the range of the controller's coordinates (the panel's when it gives none)
    #rangeY;
    #swapXY;                        // how the touch panel is mounted on the display: x and y exchanged,
    #mirrorX;                       // then x, y mirrored (in the controller's coordinates)
    #mirrorY;
    #scratch;                       // the points of the last report read: id, x, y each
    #callback = null;               // onChange(); null: none
    #timer = null;                  // the next read, planned; null: none
    #reading = false;               // a read is running
    #signalled = false;             // the interrupt came during the running read
    #lastRead = -Infinity;          // Date.now() of the last read
    #failing = false;               // the last read failed (its error printed once)
    #closed = false;

    /**
     * Opens the touch controller 'type' ("gt911", "ft6336") on 'bus' (an
     * I2c opened by the program, left open by close()) or on the bus 'port'
     * with pins 'sda' and 'scl' (opened here). The coordinates are those of
     * the drawing of 'display' (its size and rotation), or of a screen of
     * 'width' x 'height' turned by 'rotation'. 'interrupt': the pin of the
     * controller's signal, none: polled 'rate' times a second. 'swapXY',
     * 'mirrorX', 'mirrorY': how the touch panel is mounted on the display,
     * when its coordinates are not those of the display at rotation 0.
     */
    constructor({
        type, bus, port, sda, scl, address, interrupt,
        rate = DEFAULT_RATE, threshold = DEFAULT_THRESHOLD,
        display, width, height, rotation = 0,
        swapXY = false, mirrorX = false, mirrorY = false,
    } = {}) {
        // the controller
        const model = TYPES[type];
        if (model === undefined) {
            throw new RangeError(`touch: unknown type '${type}' (known: ${Object.keys(TYPES).join(", ")})`);
        }

        // the screen the coordinates are given in: the display's or the one told
        const screen = display ?? { width, height, rotation };
        const turn = ROTATIONS.indexOf(screen.rotation);
        if (turn < 0) {
            throw new RangeError("touch: rotation must be 0, 90, 180 or 270");
        }
        if (!(screen.width > 0 && screen.height > 0)) {
            throw new TypeError("touch: give the display, or its width and height");
        }
        if (!(rate > 0 && rate <= 1000)) {
            throw new RangeError("touch: rate must be above 0, 1000 at most (reads a second)");
        }

        this.#model = model;
        this.width = screen.width;
        this.height = screen.height;
        this.rotation = screen.rotation;
        this.#period = Math.round(1000 / rate);
        this.#threshold = threshold;
        this.#swapXY = swapXY === true;
        this.#mirrorX = mirrorX === true;
        this.#mirrorY = mirrorY === true;

        // the panel at rotation 0: the size exchanged when on its side
        const isOnItsSide = turn % 2 === 1;
        this.#panelWidth = isOnItsSide ? screen.height : screen.width;
        this.#panelHeight = isOnItsSide ? screen.width : screen.height;

        // the bus: the program's, or opened here
        this.#ownsBus = bus === undefined;
        this.#bus = bus ?? new I2c(port, { sda, scl, frequency: BUS_FREQUENCY });

        // the controller and what it is set to; a step that fails gives back
        // what was opened
        try {
            this.#start(address ?? model.address, interrupt);
        } catch (error) {
            this.close();
            throw error;
        }
    }

    /**
     * Opens the controller, reads its set-up, makes the points, watches the
     * interrupt pin and starts the first read (the end of the constructor).
     */
    #start(address, interrupt) {
        // the controller, and what it is set to
        this.#device = this.#bus.device(address);
        const setup = this.#model.open(this.#device);
        this.maxPoints = setup.maxPoints;

        // the range of its coordinates: its own, or the panel's (exchanged
        // when the touch panel is mounted with x and y exchanged)
        const panelRangeX = this.#swapXY ? this.#panelHeight : this.#panelWidth;
        const panelRangeY = this.#swapXY ? this.#panelWidth : this.#panelHeight;
        this.#rangeX = setup.rangeX > 0 ? setup.rangeX : panelRangeX;
        this.#rangeY = setup.rangeY > 0 ? setup.rangeY : panelRangeY;

        // the points, made once, and the scratch of the reads
        this.points = [];
        for (let i = 0; i < this.maxPoints; i++) {
            this.points.push({ id: 0, x: 0, y: 0 });
        }
        this.count = 0;
        this.#scratch = new Int32Array(this.maxPoints * POINT_ITEMS);

        // the interrupt pin: its signal starts the reads
        if (interrupt !== undefined) {
            this.#interrupt = new Gpio(interrupt, { mode: "input" });
            this.#interrupt.onChange(() => this.#wake(), { edge: "falling" });
        }

        // a first read: a finger may be on the screen already
        this.#wake();
    }

    /**
     * Calls 'callback()' each time a finger comes, goes or moves (see the
     * top of the file); null: no more calls. One callback at a time.
     */
    onChange(callback) {
        if (callback !== null && typeof callback !== "function") {
            throw new TypeError("touch: onChange takes a function or null");
        }
        this.#callback = callback;
    }

    /**
     * Stops reading; frees the interrupt pin, the controller and the bus if
     * it was opened here. Closing twice does nothing.
     */
    close() {
        this.#closed = true;
        if (this.#timer !== null) {
            clearTimeout(this.#timer);
            this.#timer = null;
        }

        this.#interrupt?.close();
        this.#device?.close();
        if (this.#ownsBus) {
            this.#bus?.close();
        }
    }

    /**
     * Something may have changed (the controller's signal, or the end of a
     * read with a finger on the screen): a read now, or once 'period' has
     * passed since the last one. Nothing when one is planned already.
     */
    #wake() {
        // closed: no more reads
        if (this.#closed) {
            return;
        }

        // a read running: what it brings may be older than this, read again after it
        if (this.#reading) {
            this.#signalled = true;
            return;
        }

        // a read planned already
        if (this.#timer !== null) {
            return;
        }

        // now, or when the period is over
        const wait = this.#lastRead + this.#period - Date.now();
        if (wait <= 0) {
            this.#read();
            return;
        }

        this.#timer = setTimeout(() => {
            this.#timer = null;
            this.#read();
        }, wait);
    }

    /**
     * Reads a report of the controller, updates the points, plans the next
     * read while needed, then calls the callback when something changed.
     */
    async #read() {
        this.#reading = true;
        this.#signalled = false;
        this.#lastRead = Date.now();

        // the report; a failure is printed once, until a read succeeds
        let changed = false;
        try {
            const bytes = await this.#device.writeRead(this.#model.request, this.#model.reportBytes(this.maxPoints));
            const found = this.#model.parse(bytes, this.#scratch, this.maxPoints);
            if (found >= 0 && this.#model.clear !== null) {
                await this.#device.write(this.#model.clear);
            }
            changed = found >= 0 && !this.#closed && this.#update(found);
            this.#failing = false;
        } catch (error) {
            if (!this.#closed && !this.#failing) {
                console.log(`touch: ${error.message}`);
            }
            this.#failing = true;
        }

        this.#reading = false;
        if (this.#closed) {
            return;
        }

        // again: a finger on the screen (its moves and its release), no
        // interrupt pin, a signal during this read, or a failed read
        const again = this.count > 0 || this.#interrupt === null || this.#signalled || this.#failing;
        if (again) {
            this.#wake();
        }

        // last: an exception of the callback stops nothing
        if (changed && this.#callback !== null) {
            this.#callback();
        }
    }

    /**
     * Takes the 'found' points of the scratch, in the coordinates of the
     * drawing; true if a finger came, went or moved by 'threshold' or more
     * (a smaller move keeps the point where it was reported).
     */
    #update(found) {
        // into the coordinates of the drawing, in the scratch
        const scratch = this.#scratch;
        for (let i = 0; i < found; i++) {
            this.#toDrawing(scratch, i * POINT_ITEMS);
        }

        // a change: another number of fingers, another finger, a move
        let changed = found !== this.count;
        for (let i = 0; i < found && !changed; i++) {
            const point = this.points[i];
            const at = i * POINT_ITEMS;
            const moved = Math.abs(scratch[at + 1] - point.x) >= this.#threshold || Math.abs(scratch[at + 2] - point.y) >= this.#threshold;
            changed = scratch[at] !== point.id || moved;
        }

        if (!changed) {
            return false;
        }

        // the points, in place
        for (let i = 0; i < found; i++) {
            const point = this.points[i];
            const at = i * POINT_ITEMS;
            point.id = scratch[at];
            point.x = scratch[at + 1];
            point.y = scratch[at + 2];
        }
        this.count = found;
        return true;
    }

    /**
     * Turns the point at 'at' of 'scratch' from the controller's coordinates
     * into those of the drawing: the mounting of the touch panel undone
     * (x and y exchanged, then mirrored), scaled to the panel, then turned
     * as the display (rotation clockwise).
     */
    #toDrawing(scratch, at) {
        // the mounting: exchanged, then mirrored, within the controller's range
        const swapped = this.#swapXY;
        const rangeX = swapped ? this.#rangeY : this.#rangeX;
        const rangeY = swapped ? this.#rangeX : this.#rangeY;
        let x = swapped ? scratch[at + 2] : scratch[at + 1];
        let y = swapped ? scratch[at + 1] : scratch[at + 2];
        if (this.#mirrorX) {
            x = rangeX - 1 - x;
        }
        if (this.#mirrorY) {
            y = rangeY - 1 - y;
        }

        // a pixel of the panel at rotation 0
        const panelWidth = this.#panelWidth;
        const panelHeight = this.#panelHeight;
        const column = Math.max(0, Math.min(panelWidth - 1, Math.floor((x * panelWidth) / rangeX)));
        const row = Math.max(0, Math.min(panelHeight - 1, Math.floor((y * panelHeight) / rangeY)));

        // turned as the drawing
        switch (this.rotation) {
            case 90: {
                scratch[at + 1] = row;
                scratch[at + 2] = panelWidth - 1 - column;
                break;
            }
            case 180: {
                scratch[at + 1] = panelWidth - 1 - column;
                scratch[at + 2] = panelHeight - 1 - row;
                break;
            }
            case 270: {
                scratch[at + 1] = panelHeight - 1 - row;
                scratch[at + 2] = column;
                break;
            }
            default: {
                scratch[at + 1] = column;
                scratch[at + 2] = row;
                break;
            }
        }
    }
}
