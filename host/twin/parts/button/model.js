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
import { Part, parsePin } from "qjsx:twin";

export default class Button extends Part {
    static controls = [
        { name: "pressed", type: "button" },
    ];

    /**
     * The pin pulled to its rest level; the button released.
     */
    constructor(config, board) {
        super(config, board);
        const activeLow = config.activeLow ?? true;
        if (typeof activeLow !== "boolean") {
            throw new Error('"activeLow" must be true or false');
        }

        this._pin = board.pin(parsePin(config.pin));
        this._activeLevel = activeLow ? 0 : 1;
        this._pin.pull(activeLow ? "up" : "down");
        this.publish({ pressed: false });
    }

    /**
     * "set" { pressed }: pressed drives the pin to its active level, released
     * lets it go.
     */
    onAction(action, args) {
        if (action !== "set" || typeof args?.pressed !== "boolean") {
            return;
        }
        this._pin.drive(args.pressed ? this._activeLevel : null);
        this.publish({ pressed: args.pressed });
    }
}
