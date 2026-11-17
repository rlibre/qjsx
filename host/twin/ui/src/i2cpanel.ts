/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What the section of an I2C bus shows: the trace of the transfers of the
// program on it (tracepanel.ts), one line each: when, the address, the bytes
// written and read (hexadecimal), and the result (NACK, timeout...). The
// section is shown only for a bus the program opened with no part wired on
// its pins (a part has a section of its own: boardview.ts).
import { BoxProps } from "x4js";
import { frequencyText } from "./frequency";
import { I2cBusState, I2cRecord } from "./messages";
import { tx } from "./texts";
import { asHex, timeText, TracePanel } from "./tracepanel";

/**
 * The title line of the section of a bus: "closed", or "sda 8 · scl 9 · 100
 * kHz" (the frequencies of its devices, once the program made some).
 */
export function i2cSummary(state: I2cBusState) {
    // closed: only that
    if (!state.open) {
        return tx.twin.i2c_closed;
    }

    // its pins, then the frequencies of its devices, if any
    const pins = `sda ${state.sda} · scl ${state.scl}`;
    const frequencies = state.frequencies.map(frequencyText).join(", ");
    return frequencies === "" ? pins : `${pins} · ${frequencies}`;
}

/**
 * The text of a transfer: "12.345 s  0x76  100 kHz  W D0  R 60", and its
 * result when it is not ok; "probe" for a transfer of nothing.
 */
function recordText(record: I2cRecord) {
    // when, to whom, at what frequency (a probe has none)
    const address = `0x${record.address.toString(16).padStart(2, "0")}`;
    const parts = [timeText(record.time), address];
    if (record.frequency > 0) {
        parts.push(frequencyText(record.frequency));
    }

    // what was written and read (a probe: nothing)
    const isProbe = record.writeSize === 0 && record.readSize === 0;
    if (isProbe) {
        parts.push(tx.twin.i2c_probe);
    }

    if (record.writeSize > 0) {
        parts.push(`W ${asHex(record.written, record.writeSize)}`);
    }

    const hasRead = record.readSize > 0 && record.status === "ok";
    if (hasRead) {
        parts.push(`R ${asHex(record.read, record.readSize)}`);
    }

    // the result, when it went wrong
    if (record.status !== "ok") {
        parts.push(record.status.toUpperCase());
    }

    return parts.join("  ");
}

export class I2cPanel extends TracePanel {
    /**
     * An empty trace, and the settings button its section shows.
     */
    constructor(props: BoxProps) {
        super(props, tx.twin.i2c_menu_tooltip, tx.twin.i2c_clear);
    }

    /**
     * Adds transfers to the trace (a transfer that did not go through has the
     * class of its status).
     */
    addRecords(records: I2cRecord[]) {
        this.addLines(records.map(record => ({ text: recordText(record), cls: record.status })));
    }
}
