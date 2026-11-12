/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The console of the parts: the lines their models log (part.log) and the
// errors of their code (twin.log), the newest at the bottom, MAX_LINES kept.
import { BoxProps, Button, Flex, HBox, Label, VBox } from "x4js";
import { LogLine } from "./messages";
import { tx } from "./texts";

// Lines kept; the oldest go first.
const MAX_LINES = 500;

export class ConsoleView extends VBox {
    declare protected refs: { lines: VBox };

    private _lines: Label[] = [];   // the lines shown, oldest first

    /**
     * An empty console; its Clear button in a status bar at the bottom.
     */
    constructor(props: BoxProps) {
        super(props);

        // the lines, then a status bar under them
        this.setContent([
            this.refs.lines = new VBox({ cls: "console-lines", flex: true }),
            new HBox({ cls: "console-bar", content: [
                new Flex(),
                new Button({ cls: "outline", label: tx.twin.console_clear, click: () => this.clear() }),
            ] }),
        ]);
    }

    /**
     * A line of a part, at the bottom (scrolled into view).
     */
    addLine(line: LogLine) {
        // the line: "[id] text"
        const label = new Label({ cls: "console-line", text: `[${line.id ?? "?"}] ${line.text}` });
        this._lines.push(label);
        this.refs.lines.appendContent(label);

        // the oldest ones beyond MAX_LINES
        while (this._lines.length > MAX_LINES) {
            this.refs.lines.removeChild(this._lines.shift());
        }
        label.scrollIntoView();
    }

    /**
     * No line any more.
     */
    clear() {
        this._lines = [];
        this.refs.lines.clearContent();
    }
}
