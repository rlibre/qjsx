/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A potentiometer between the ground and the supply of the board, its wiper
// on a pin: the voltage on the pin follows its position (0 to 100 %), read
// by device/adc. A knob on the page turns it (the table of the pins shows
// the voltage).
//
//   { "id": "gain", "model": "potentiometer", "label": "Gain", "pin": 4, "position": 50 }
//
// "position": where it starts, in % (50 by default).
import { Part, parsePin } from "qjsx:twin";

export default class Potentiometer extends Part {
    static controls = [
        { name: "position", type: "knob", label: "", min: 0, max: 100, step: 1, unit: "%" },
    ];

    /**
     * The wiper on its pin, at its first position.
     */
    constructor(config, board) {
        super(config, board);
        const position = config.position ?? 50;
        if (typeof position !== "number" || position < 0 || position > 100) {
            throw new Error('"position" must be 0 to 100 (%)');
        }

        this._pin = board.pin(parsePin(config.pin));
        this._turn(position);
    }

    /**
     * "set" { position }: the wiper moves.
     */
    onAction(action, args) {
        if (action === "set" && typeof args?.position === "number") {
            this._turn(Math.min(100, Math.max(0, args.position)));
        }
    }

    /**
     * The wiper at 'position' %: the voltage on the pin, then the state.
     */
    _turn(position) {
        const voltage = (this.board.supply * position) / 100;
        this._pin.voltage = voltage;
        this.publish({ position, voltage });
    }
}
