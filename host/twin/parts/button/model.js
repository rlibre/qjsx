/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A push button between a pin and the ground (active low, the default: the
// board pulls the pin up) or the supply (activeLow: false: pulled down).
// Held down on the page, it drives the pin to its active level.
//
//   { "id": "boot", "model": "button", "label": "BOOT", "pin": 0, "activeLow": true }
//
// Or the reset button of the board ("reset": true, no pin): released, it
// restarts the device, as the EN pin of an ESP32 (the chip is held in reset
// while it is down, and starts when it is released).
//
//   { "id": "reset", "model": "button", "label": "RESET", "reset": true }
import { Part, parsePin } from "qjsx:twin";

export default class Button extends Part {
    static controls = [
        { name: "pressed", type: "button" },
    ];

    /**
     * The pin pulled to its rest level, or none for the reset button; the
     * button released.
     */
    constructor(config, board) {
        super(config, board);

        // its fields: activeLow (true by default), reset (false by default)
        const activeLow = config.activeLow ?? true;
        const reset = config.reset ?? false;
        if (typeof activeLow !== "boolean") {
            throw new Error('"activeLow" must be true or false');
        }

        if (typeof reset !== "boolean") {
            throw new Error('"reset" must be true or false');
        }

        // the reset button: no pin
        this._pressed = false;
        this._pin = null;
        if (reset) {
            if (config.pin !== undefined) {
                throw new Error('the reset button has no "pin"');
            }
            this.publish({ pressed: false });
            return;
        }

        // a button on a pin: the pin pulled to its rest level
        this._pin = board.pin(parsePin(config.pin));
        this._activeLevel = activeLow ? 0 : 1;
        this._pin.pull(activeLow ? "up" : "down");
        this.publish({ pressed: false });
    }

    /**
     * "set" { pressed }: pressed drives the pin to its active level, released
     * lets it go; the reset button restarts the device when released.
     */
    onAction(action, args) {
        // "set" with a boolean only
        const isNotSet = action !== "set" || typeof args?.pressed !== "boolean";
        if (isNotSet) {
            return;
        }

        // released after a press: the reset button restarts the device; a
        // button on a pin drives it while pressed
        const released = this._pressed && !args.pressed;
        this._pressed = args.pressed;
        if (this._pin === null) {
            if (released) {
                this.board.restart();
            }
        } else {
            this._pin.drive(args.pressed ? this._activeLevel : null);
        }

        this.publish({ pressed: args.pressed });
    }
}
