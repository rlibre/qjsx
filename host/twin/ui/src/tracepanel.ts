/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The trace of a bus in its section (an I2C bus: i2cpanel.ts, an SPI bus:
// spipanel.ts): one line per transfer of the program, the newest in view,
// the oldest dropped past MAX_LINES. Clearing it is in the menu of a
// settings button the section shows on its title line (menuButton), as for
// a UART. Also the texts its lines share: bytes in hexadecimal, times.
import { BoxProps, Button, Label, VBox } from "x4js";
import { LogView } from "./controls/logview";

import gearIcon from "./assets/gear.svg";

// Lines kept in a trace; the oldest go first.
const MAX_LINES = 500;

// A line of a trace: its text, and a class for its look ("nack", "timeout"...).
export interface TraceLine {
    text: string;
    cls:  string;
}

/**
 * Bytes in hexadecimal: "D0 60"; when the trace kept fewer than 'size',
 * "…" and the size after them: "D0 60 … 1024 B".
 */
export function asHex(bytes: number[], size: number) {
    const text = bytes.map(byte => byte.toString(16).toUpperCase().padStart(2, "0")).join(" ");
    const isCut = size > bytes.length;
    return isCut ? `${text} … ${size} B` : text;
}

/**
 * Milliseconds since the twin started, as "12.345 s".
 */
export function timeText(ms: number) {
    return `${(ms / 1000).toFixed(3)} s`;
}

export class TracePanel extends VBox {
    declare protected refs: { log: LogView };

    private _lines: Label[] = [];   // the lines shown, oldest first

    // The settings button, for the title line of the section: its menu
    // clears the trace.
    readonly menuButton: Button;

    /**
     * An empty trace, and the settings button its section shows ('tooltip'
     * of the button, 'clearText' of its menu item).
     */
    constructor(props: BoxProps, tooltip: string, clearText: string) {
        super({ ...props, cls: `trace-panel ${props.cls ?? ""}` });

        // the settings button: its menu clears the trace
        this.menuButton = new Button({
            cls: "outline menu-button",
            icon: gearIcon,
            tooltip,
            menu: () => [{ text: clearText, click: () => this.clear() }],
        });

        // the trace
        this.setContent([
            this.refs.log = new LogView({ cls: "trace-log" }),
        ]);
    }

    /**
     * Adds lines to the trace, the newest in view unless the user scrolled
     * up (LogView).
     */
    addLines(lines: TraceLine[]) {
        // the lines
        for (const { text, cls } of lines) {
            const line = new Label({ cls: `trace-line ${cls}`, text });
            this._lines.push(line);
            this.refs.log.appendContent(line);
        }

        // the oldest dropped past MAX_LINES
        while (this._lines.length > MAX_LINES) {
            this.refs.log.removeChild(this._lines.shift());
        }

        // the newest in view
        this.refs.log.followNewest();
    }

    /**
     * Empties the trace.
     */
    clear() {
        this._lines = [];
        this.refs.log.clearLines();
    }
}
