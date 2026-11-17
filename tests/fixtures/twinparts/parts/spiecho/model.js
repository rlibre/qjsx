/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A part of the project (tests/twinparts.test.js): an SPI device that
// answers each read with the bytes last written (a transfer gives them, then
// keeps what it sent), counts the transactions and logs each one with the
// level of the DC pin of its writes: "transaction C1 D3 w10 R3" (C a write
// with DC low, D with DC high, R a read, X a transfer, w a delay in µs).
import { Part } from "qjsx:twin";

/**
 * The short text of a segment for the log.
 */
function segmentText(segment) {
    switch (segment.kind) {
        case "write": {
            const letter = segment.dc === 0 ? "C" : "D";
            return `${letter}${segment.size}`;
        }
        case "read": {
            return `R${segment.size}`;
        }
        case "transfer": {
            return `X${segment.size}`;
        }
        default: {
            return `w${segment.delayUs}`;
        }
    }
}

export default class SpiEcho extends Part {
    /**
     * Sits on its chip select of its controller.
     */
    constructor(config, board) {
        super(config, board);
        this._last = new Uint8Array(0);   // the bytes last written
        this._transactions = 0;           // transactions heard
        board.spi(config.spi).attach(config.spi.cs, this);
        this.publish({ transactions: 0 });
    }

    /**
     * A transaction of the program: each read gives the bytes last written;
     * each write (and transfer) keeps what it sent.
     */
    onTransaction(segments) {
        // in the order of the segments: answer, then keep what was sent
        const answers = [];

        for (const segment of segments) {
            const reads = segment.kind === "read" || segment.kind === "transfer";
            if (reads) {
                answers.push(this._last);
            }

            if (segment.bytes !== undefined) {
                this._last = segment.bytes;
            }
        }

        // counted and logged
        this._transactions++;
        this.publish({ transactions: this._transactions });
        this.log(`transaction ${segments.map(segmentText).join(" ")}`);
        return answers;
    }
}
