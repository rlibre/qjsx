/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A line of WS2812 RGB LEDs on a pin: the program sends their colors with
// device/rmt (sendBytes, 3 bytes per LED in the order of the LED); the model
// decodes them and publishes { colors: [[r, g, b], ...] }.
//
//   { "id": "rgb", "model": "ws2812", "label": "RGB", "pin": 48, "count": 1 }
//
// "order": the byte order of the LED, "grb" for a WS2812 (the default).
import { Part, parsePin } from "qjsx:twin";

export default class Ws2812 extends Part {
    static controls = [
        { name: "colors", type: "led" },
    ];

    /**
     * The line, all off; listens to what the RMT sends on its pin.
     */
    constructor(config, board) {
        super(config, board);
        const count = config.count ?? 1;
        const order = config.order ?? "grb";
        if (!Number.isInteger(count) || count < 1 || count > 1024) {
            throw new Error('"count" must be 1 to 1024');
        }
        if (typeof order !== "string" || [...order].sort().join("") !== "bgr") {
            throw new Error('"order" must be the three letters r, g, b in some order ("grb")');
        }

        this._count = count;
        this._order = order;
        board.rmt(parsePin(config.pin)).onBytes(bytes => this._show(bytes));
        this.publish({ colors: new Array(count).fill([0, 0, 0]) });
    }

    /**
     * The bytes of a send: 3 per LED in the order of the LED; the LEDs past
     * the bytes are off.
     */
    _show(bytes) {
        const colors = [];
        for (let index = 0; index < this._count; index++) {
            const offset = index * 3;
            if (offset + 3 > bytes.length) {
                colors.push([0, 0, 0]);
                continue;
            }

            const color = { r: 0, g: 0, b: 0 };
            for (let channel = 0; channel < 3; channel++) {
                color[this._order[channel]] = bytes[offset + channel];
            }
            colors.push([color.r, color.g, color.b]);
        }
        this.publish({ colors });
    }
}
