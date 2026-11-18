/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A FocalTech FT6336 touch controller on an I2C bus, laid over a screen part:
// the page touches the screen (one finger, the mouse), the program reads the
// controller as on a real board. Enough for a driver written for the chip:
//   - registers (8-bit, the pointer moves at each byte): 0x02 the number of
//     points, then 6 bytes a point from 0x03 (point 2 from 0x09): event (2
//     high bits: 0 down, 1 up, 2 contact) and x bits 11-8, x bits 7-0, id
//     (4 high bits) and y bits 11-8, y bits 7-0, weight, area; 0xA3 the chip
//     id (0x64), 0xA6 the firmware, 0xA8 the vendor id (0x11, FocalTech);
//     what the program writes is kept, nothing else happens;
//   - its coordinates are the pixels of the screen's glass (the board held
//     USB connector at the bottom, as the WT32-SC01's);
//   - the interrupt pin, when wired, low while a finger is on the screen (its
//     "polling" mode, the chip's default), let go otherwise (the board's
//     pull-up).
//
//   { "id": "touch", "model": "ft6336", "label": "Touch", "i2c": { "sda": 18, "scl": 19 },
//     "address": "0x38", "interrupt": 39, "screen": "screen" }
//
// The page sends "press", "move" with { x, y } (pixels of the glass), and
// "release". The state published: { screen, pressed, x, y }.
import { Part, Registers, parseAddress, parsePin } from "qjsx:twin";

// Registers.
const REG_POINTS = 0x02;               // number of points
const REG_POINT_1 = 0x03;              // 6 bytes
const REG_CHIP_ID = 0xa3;
const REG_FIRMWARE = 0xa6;
const REG_VENDOR = 0xa8;

const CHIP_ID = 0x64;
const FIRMWARE = 0x10;
const VENDOR = 0x11;

// The event of a point (2 high bits of its first byte).
const EVENT_DOWN = 0;
const EVENT_UP = 1;
const EVENT_CONTACT = 2;

// The finger: its id and weight, as the chip gives them.
const FINGER_ID = 0;
const WEIGHT = 0x20;

// The largest coordinate the chip can give (12 bits).
const MOST_COORDINATE = 0xfff;

export default class Ft6336 extends Part {
    /**
     * The registers on the bus, the interrupt pin let go, nobody touching.
     */
    constructor(config, board) {
        super(config, board);

        // the screen it covers
        if (typeof config.screen !== "string") {
            throw new Error('"screen" must be the id of the screen part the touch panel covers');
        }

        // the registers, at power on
        const address = parseAddress(config.address ?? 0x38);
        this._registers = new Registers(256);
        this._registers.set(REG_CHIP_ID, CHIP_ID);
        this._registers.set(REG_FIRMWARE, FIRMWARE);
        this._registers.set(REG_VENDOR, VENDOR);
        board.i2c(config.i2c).attach(address, this._registers);

        // the interrupt pin, high by the board's pull-up
        this._interrupt = null;
        if (config.interrupt !== undefined) {
            this._interrupt = board.pin(parsePin(config.interrupt, "interrupt"));
            this._interrupt.pull("up");
        }

        this._pressed = false;
        this._x = 0;
        this._y = 0;
        this._publishState();
    }

    /**
     * "press" / "move" with { x, y }: a finger there; "release": no more.
     */
    onAction(action, args) {
        switch (action) {
            case "press":
            case "move": {
                const isPoint = typeof args === "object" && args !== null && Number.isFinite(args.x) && Number.isFinite(args.y);
                if (isPoint) {
                    this._touch(action === "press" || !this._pressed ? EVENT_DOWN : EVENT_CONTACT, args.x, args.y);
                }
                break;
            }
            case "release": {
                this._release();
                break;
            }
        }
    }

    /**
     * A finger at (x, y): one point in the registers, the interrupt low.
     */
    _touch(event, x, y) {
        // the point, its coordinates on 12 bits
        this._x = Math.max(0, Math.min(MOST_COORDINATE, Math.round(x)));
        this._y = Math.max(0, Math.min(MOST_COORDINATE, Math.round(y)));
        this._pressed = true;
        this._registers.set(REG_POINTS, 1);
        this._registers.set(REG_POINT_1, [
            (event << 6) | (this._x >> 8),
            this._x & 0xff,
            (FINGER_ID << 4) | (this._y >> 8),
            this._y & 0xff,
            WEIGHT,
            0,
        ]);

        // the program told: the interrupt low while touched
        this._interrupt?.drive(0);
        this._publishState();
    }

    /**
     * The finger lifted: no point, its event "up", the interrupt let go.
     */
    _release() {
        if (!this._pressed) {
            return;
        }

        this._pressed = false;
        this._registers.set(REG_POINTS, 0);
        this._registers.set(REG_POINT_1, (EVENT_UP << 6) | (this._x >> 8));
        this._interrupt?.drive(null);
        this._publishState();
    }

    /**
     * The state for the page: the screen covered (the page links the two by
     * it), the finger in pixels of the glass.
     */
    _publishState() {
        this.publish({ screen: this.config.screen, pressed: this._pressed, x: this._x, y: this._y });
    }
}
