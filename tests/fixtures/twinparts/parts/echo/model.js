/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A part of the project (tests/twinparts.test.js): an I2C device that gives
// back the bytes last written, reversed, counts the transfers and logs each
// one.
import { Part } from "qjsx:twin";

export default class Echo extends Part {
    /**
     * Answers at its address on its bus.
     */
    constructor(config, board) {
        super(config, board);
        this._last = new Uint8Array(0);
        this._transfers = 0;
        board.i2c(config.i2c).attach(config.address, this);
        this.publish({ transfers: 0 });
    }

    /**
     * A transfer of the program: keeps what it wrote, reversed; reads it.
     */
    onTransfer(written, readLength) {
        if (written.length > 0) {
            this._last = written.slice().reverse();
        }
        this._transfers++;
        this.publish({ transfers: this._transfers });
        this.log(`transfer ${written.length}/${readLength}`);

        const answer = new Uint8Array(readLength);
        answer.set(this._last.subarray(0, readLength));
        return answer;
    }
}
