/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What the section of an I2C bus shows: the trace of the transfers of the
// program on it, one line each: when, the address, the bytes written and
// read (hexadecimal), and the result (NACK, timeout...). The section is
// shown only for a bus the program opened with no part wired on its pins
// (a part has a section of its own: boardview.ts).
import { BoxProps, Button, Flex, HBox, Label, VBox } from "x4js";
import { I2cBusState, I2cRecord } from "./messages";
import { tx } from "./texts";

// Lines kept in the trace; the oldest go first.
const MAX_LINES = 500;

/**
 * A frequency as text: "100 kHz", "1 MHz".
 */
function frequencyText(hz: number) {
    return hz >= 1000000 ? `${hz / 1000000} MHz` : `${hz / 1000} kHz`;
}

/**
 * The title line of the section of a bus: "closed", or "sda 8 · scl 9 · 100
 * kHz" (the frequencies of its devices, once the program made some).
 */
export function i2cSummary(state: I2cBusState) {
    if (!state.open) {
        return tx.twin.i2c_closed;
    }
    const pins = `sda ${state.sda} · scl ${state.scl}`;
    const frequencies = state.frequencies.map(frequencyText).join(", ");
    return frequencies === "" ? pins : `${pins} · ${frequencies}`;
}

/**
 * Bytes in hexadecimal: "D0 60"; "…" after them when there were more than
 * the trace keeps.
 */
function asHex(bytes: number[], size: number) {
    const text = bytes.map(byte => byte.toString(16).toUpperCase().padStart(2, "0")).join(" ");
    return size > bytes.length ? `${text} …` : text;
}

/**
 * Milliseconds since the twin started, as "12.345 s".
 */
function timeText(ms: number) {
    return `${(ms / 1000).toFixed(3)} s`;
}

/**
 * The text of a transfer: "12.345 s  0x76  100 kHz  W D0  R 60", and its
 * result when it is not ok; "probe" for a transfer of nothing.
 */
function recordText(record: I2cRecord) {
    // what was written and read
    const address = `0x${record.address.toString(16).padStart(2, "0")}`;
    const parts = [timeText(record.time), address];
    if (record.frequency > 0) {
        parts.push(frequencyText(record.frequency));
    }
    if (record.writeSize === 0 && record.readSize === 0) {
        parts.push(tx.twin.i2c_probe);
    }
    if (record.writeSize > 0) {
        parts.push(`W ${asHex(record.written, record.writeSize)}`);
    }
    if (record.readSize > 0 && record.status === "ok") {
        parts.push(`R ${asHex(record.read, record.readSize)}`);
    }

    // the result, when it went wrong
    if (record.status !== "ok") {
        parts.push(record.status.toUpperCase());
    }
    return parts.join("  ");
}

export class I2cPanel extends VBox {
    declare protected refs: { log: VBox };

    private _lines: Label[] = [];   // the lines shown, oldest first

    /**
     * An empty trace; Clear in the bar above it.
     */
    constructor(props: BoxProps) {
        super({ ...props, cls: `i2c-panel ${props.cls ?? ""}` });

        this.setContent([
            new HBox({ cls: "i2c-bar", content: [
                new Label({ cls: "i2c-log-title", text: tx.twin.i2c_trace }),
                new Flex(),
                new Button({ cls: "outline", label: tx.twin.uart_clear, click: () => this.clear() }),
            ] }),
            this.refs.log = new VBox({ cls: "i2c-log" }),
        ]);
    }

    /**
     * Adds transfers to the trace, the newest in view.
     */
    addRecords(records: I2cRecord[]) {
        // the lines, the oldest dropped past MAX_LINES
        for (const record of records) {
            const line = new Label({ cls: `i2c-line ${record.status}`, text: recordText(record) });
            this._lines.push(line);
            this.refs.log.appendContent(line);
        }
        while (this._lines.length > MAX_LINES) {
            this.refs.log.removeChild(this._lines.shift());
        }

        // the newest in view
        this._lines[this._lines.length - 1]?.scrollIntoView({ block: "nearest" });
    }

    /**
     * Empties the trace.
     */
    clear() {
        this._lines = [];
        this.refs.log.clearContent();
    }
}
