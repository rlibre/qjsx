/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A Goodix GT911 touch controller on an I2C bus, laid over a screen part:
// the page touches the screen (one finger, the mouse), the program reads the
// controller as on a real board. Enough for a driver written for the chip:
//   - 16-bit register addresses, high byte first, then the bytes written
//     from there (the pointer moves at each byte); a read gives the bytes
//     from the pointer on;
//   - 0x8047 the configuration: its version, the range of x and y
//     (little-endian), the most points, the trigger of the interrupt
//     (falling edge); 0x8140 the product id "911", 0x8144 the firmware;
//   - 0x814E the status: bit 7 a new report, bits 0-3 its points, then 8
//     bytes a point from 0x814F (id, x, y little-endian, size, a reserved
//     byte). While a finger is on the screen the chip makes a report every
//     10 ms, but only once the program cleared the status (wrote 0 there):
//     until then the report stays. The last report says no point. The
//     reports the program has not had yet wait in a queue, in order (a
//     press and its release within one tick both reach it); a move
//     replaces the finger's report still waiting;
//   - the interrupt pin, when wired, pulses low at each new report;
//   - it starts configured: its reset sequence (reset pin, interrupt pin
//     choosing the address) is not played.
//
//   { "id": "touch", "model": "gt911", "label": "Touch", "i2c": { "sda": 31, "scl": 32 },
//     "address": "0x14", "interrupt": 23, "screen": "screen",
//     "range": [320, 480], "points": 5 }
//
// "range" is the range of its coordinates (default the 320 x 480 glass of
// the screen part st7796-320x480): a touch at (x, y) of the glass is given
// scaled to it. "points": the most points of its configuration (1 to 10,
// default 5; one finger is ever on the screen).
//
// The page sends "press", "move" with { x, y, width, height } (pixels of
// the glass and its size), and "release". The state published: { screen,
// pressed, x, y }.
import { Part, parseAddress, parsePin } from "qjsx:twin";

// The registers held: 0x8040 to 0x817F.
const FIRST_REGISTER = 0x8040;
const REGISTER_COUNT = 0x140;

const REG_CONFIG_VERSION = 0x8047;
const REG_X_RANGE = 0x8048;            // 2 bytes, little-endian
const REG_Y_RANGE = 0x804a;
const REG_MOST_POINTS = 0x804c;
const REG_MODULE_SWITCH_1 = 0x804d;    // bits 0-1: the trigger of the interrupt
const REG_PRODUCT_ID = 0x8140;         // 4 bytes
const REG_FIRMWARE = 0x8144;           // 2 bytes
const REG_STATUS = 0x814e;
const REG_POINT_1 = 0x814f;            // 8 bytes a point

const CONFIG_VERSION = 0x50;
const TRIGGER_FALLING = 0x01;
const PRODUCT_ID = [0x39, 0x31, 0x31, 0x00];   // "911"
const FIRMWARE = [0x60, 0x10];
const NEW_REPORT = 0x80;

// How often it reports while a finger is on the screen, and the pulse.
const REPORT_MS = 10;
const PULSE_MS = 1;

// The finger: its id and size.
const FINGER_ID = 0;
const SIZE = 0x20;

export default class Gt911 extends Part {
    /**
     * The registers on the bus, configured; the interrupt pin let go; nobody
     * touching.
     */
    constructor(config, board) {
        super(config, board);

        // the screen it covers, the range of its coordinates, its most points
        if (typeof config.screen !== "string") {
            throw new Error('"screen" must be the id of the screen part the touch panel covers');
        }

        const [rangeX, rangeY] = config.range ?? [320, 480];
        const isRange = Number.isInteger(rangeX) && Number.isInteger(rangeY) && rangeX > 0 && rangeY > 0 && rangeX < 0x10000 && rangeY < 0x10000;
        if (!isRange) {
            throw new Error('"range" must be [width, height], integers from 1 to 65535');
        }

        const points = config.points ?? 5;
        if (!Number.isInteger(points) || points < 1 || points > 10) {
            throw new Error('"points" must be an integer from 1 to 10');
        }

        this._rangeX = rangeX;
        this._rangeY = rangeY;

        // the registers, configured, on the bus
        this._registers = new Uint8Array(REGISTER_COUNT);
        this._pointer = FIRST_REGISTER;
        this._set(REG_CONFIG_VERSION, [CONFIG_VERSION]);
        this._set(REG_X_RANGE, [rangeX & 0xff, rangeX >> 8]);
        this._set(REG_Y_RANGE, [rangeY & 0xff, rangeY >> 8]);
        this._set(REG_MOST_POINTS, [points]);
        this._set(REG_MODULE_SWITCH_1, [TRIGGER_FALLING]);
        this._set(REG_PRODUCT_ID, PRODUCT_ID);
        this._set(REG_FIRMWARE, FIRMWARE);
        const address = parseAddress(config.address ?? 0x14);
        board.i2c(config.i2c).attach(address, { onTransfer: (written, readLength) => this._onTransfer(written, readLength) });

        // the interrupt pin, high by the board's pull-up
        this._interrupt = null;
        if (config.interrupt !== undefined) {
            this._interrupt = board.pin(parsePin(config.interrupt, "interrupt"));
            this._interrupt.pull("up");
        }

        this._pressed = false;
        this._x = 0;                // in its range
        this._y = 0;
        this._glassX = 0;           // as the page gave it
        this._glassY = 0;
        this._ticker = null;        // the reports while touched (and the last one)
        this._queue = [];           // reports the program has not had yet, in order: { points: 0 | 1, x, y }
        this._publishState();
    }

    /**
     * "press" / "move" with { x, y, width, height }: a finger there;
     * "release": no more.
     */
    onAction(action, args) {
        switch (action) {
            case "press":
            case "move": {
                this._touch(args);
                break;
            }
            case "release": {
                this._release();
                break;
            }
        }
    }

    /**
     * A finger at (x, y) of the glass: scaled to the range, reported from
     * now on every REPORT_MS.
     */
    _touch(args) {
        // a point of a glass of some size
        const isPoint = typeof args === "object" && args !== null
            && Number.isFinite(args.x) && Number.isFinite(args.y) && args.width > 0 && args.height > 0;
        if (!isPoint) {
            return;
        }

        // scaled to the range
        this._glassX = Math.round(args.x);
        this._glassY = Math.round(args.y);
        this._x = Math.max(0, Math.min(this._rangeX - 1, Math.floor((args.x * this._rangeX) / args.width)));
        this._y = Math.max(0, Math.min(this._rangeY - 1, Math.floor((args.y * this._rangeY) / args.height)));
        this._pressed = true;

        // a report of the finger queued (a move replaces the finger's report
        // not had yet: the program gets the place of the moment)
        const last = this._queue[this._queue.length - 1];
        if (last !== undefined && last.points === 1) {
            last.x = this._x;
            last.y = this._y;
        } else {
            this._queue.push({ points: 1, x: this._x, y: this._y });
        }

        // the reports, from now on every REPORT_MS; one at once if it can
        if (this._ticker === null) {
            this._ticker = setInterval(() => this._report(), REPORT_MS);
        }
        this._report();
        this._publishState();
    }

    /**
     * The finger lifted: a report without point queued, after the finger's.
     */
    _release() {
        if (!this._pressed) {
            return;
        }

        this._pressed = false;
        this._queue.push({ points: 0, x: 0, y: 0 });
        this._report();
        this._publishState();
    }

    /**
     * A report, when the program cleared the status: the oldest queued
     * (a press and its release both reach the program, even within one
     * tick: two clicks stay two touches), else the finger still there;
     * nothing left, the reports stop. A pulse on the interrupt pin.
     */
    _report() {
        // the program has not taken the last report yet: it stays
        if ((this._get(REG_STATUS) & NEW_REPORT) !== 0) {
            return;
        }

        // the oldest report queued, else the finger held, else the end
        let report = this._queue.shift();
        if (report === undefined && this._pressed) {
            report = { points: 1, x: this._x, y: this._y };
        }

        if (report === undefined) {
            clearInterval(this._ticker);
            this._ticker = null;
            return;
        }

        if (report.points === 1) {
            const { x, y } = report;
            this._set(REG_POINT_1, [FINGER_ID, x & 0xff, x >> 8, y & 0xff, y >> 8, SIZE, 0, 0]);
            this._set(REG_STATUS, [NEW_REPORT | 1]);
        } else {
            this._set(REG_STATUS, [NEW_REPORT]);
        }

        // the pulse
        if (this._interrupt !== null) {
            this._interrupt.drive(0);
            setTimeout(() => this._interrupt.drive(null), PULSE_MS);
        }
    }

    /**
     * A transfer of the program: the register address (2 bytes), the bytes
     * written from there, then the bytes read from the pointer on (0 outside
     * the registers held).
     */
    _onTransfer(written, readLength) {
        // the pointer, then the bytes written
        if (written.length >= 2) {
            this._pointer = (written[0] << 8) | written[1];
            for (let index = 2; index < written.length; index++) {
                this._set(this._pointer, [written[index]]);
                this._pointer++;
            }
        }

        // the read
        const answer = new Uint8Array(readLength);
        for (let index = 0; index < readLength; index++) {
            answer[index] = this._get(this._pointer);
            this._pointer++;
        }

        return answer;
    }

    /**
     * The value of a register; 0 outside the registers held.
     */
    _get(register) {
        const at = register - FIRST_REGISTER;
        return at >= 0 && at < REGISTER_COUNT ? this._registers[at] : 0;
    }

    /**
     * Sets the registers from 'register' on; those outside the registers
     * held are ignored.
     */
    _set(register, values) {
        values.forEach((value, index) => {
            const at = register + index - FIRST_REGISTER;
            if (at >= 0 && at < REGISTER_COUNT) {
                this._registers[at] = value & 0xff;
            }
        });
    }

    /**
     * The state for the page: the screen covered (the page links the two by
     * it), the finger in pixels of the glass.
     */
    _publishState() {
        this.publish({ screen: this.config.screen, pressed: this._pressed, x: this._glassX, y: this._glassY });
    }
}
