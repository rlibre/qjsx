/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The table of the pins: level, mode, pull, part, and buttons to force a pin
// to 0 or 1 or let it go. One row per pin, built once and updated in place
// (a click is never lost to a rebuild).
import { BoxProps, Button, HBox, Header, Label, StateProxy, VBox } from "x4js";
import { frequencyText, percentText } from "./frequency";
import { BoardInfo, forcePin, PartInfo, PinState } from "./messages";
import { TwinSettings } from "./settings";
import { tx } from "./texts";

// The levels the force buttons send, in order: on (1), off (0), let go.
const FORCE_LEVELS = [1, 0, -1];

/**
 * The text of the force button of 'level' (read when a row is built: the
 * texts are selected by then).
 */
function forceText(level: number) {
    if (level < 0) {
        return tx.twin.free;
    }
    return level === 1 ? tx.twin.force_on : tx.twin.force_off;
}

/**
 * The tooltip of the force button of 'level': only "free" needs one.
 */
function forceTooltip(level: number) {
    return level < 0 ? tx.twin.free_tooltip : undefined;
}

/**
 * What the program uses a pin for: an analog input with its voltage ("adc
 * 1.65 V"), a PWM output ("pwm 1 kHz 25 %", "inv" when active low), a bus
 * or a channel ("i2c", "rmt"), else its gpio mode, "-" when it does not use
 * it.
 */
export function pinMode(state: PinState) {
    if (state.adc) {
        return `adc ${state.voltage.toFixed(2)} V`;
    }
    if (state.pwm !== null) {
        const inverted = state.pwm.invert ? " inv" : "";
        return `pwm ${frequencyText(state.pwm.frequency)} ${percentText(state.pwm.duty)}${inverted}`;
    }
    if (state.i2c) {
        return "i2c";
    }
    if (state.rmt) {
        return "rmt";
    }
    return state.configured ? state.mode : "-";
}

class PinRow extends HBox {
    declare protected refs: { level: Label; mode: Label; pull: Label; part: Label };

    private _buttons: Button[] = [];   // the force buttons, in the order of FORCE_LEVELS

    /**
     * The row of 'pin'; the parts on it come with setPart().
     */
    constructor(pin: number) {
        super({ cls: "pin-row" });

        // the force buttons
        this._buttons = FORCE_LEVELS.map(level => {
            return new Button({
                cls: "outline",
                label: forceText(level),
                tooltip: forceTooltip(level),
                click: () => forcePin(pin, level),
            });
        });

        // a cell per column of the header (the dot of the level in its own)
        this.setContent([
            new Label({ cls: "number", text: String(pin) }),
            new HBox({ cls: "level-cell", content: this.refs.level = new Label({ cls: "level", text: "" }) }),
            this.refs.mode = new Label({ cls: "mode", text: "" }),
            this.refs.pull = new Label({ cls: "pull", text: "" }),
            this.refs.part = new Label({ cls: "part-name", text: "" }),
            new HBox({ cls: "force", content: this._buttons }),
        ]);
    }

    /**
     * The labels of the parts on the pin ("" if none).
     */
    setPart(labels: string) {
        this.refs.part.setText(labels);
    }

    /**
     * Shows the state of the pin.
     */
    update(state: PinState) {
        // level (its color) and configuration; a part drives it, or fights
        // the program (a short on a real board)
        this.refs.level.setClass("high", state.level === 1);
        this.refs.mode.setText(pinMode(state));
        this.refs.pull.setText(state.configured ? state.pull : "-");
        this.setClass("driven", state.partDrive !== null);
        this.setClass("conflict", state.conflict);
        this.refs.level.setAttribute("tooltip", state.conflict ? tx.twin.conflict_tooltip : null);

        // the force button that matches is filled, the others outlined
        const current = state.forced ?? -1;
        this.setClass("forced", state.forced !== null);
        FORCE_LEVELS.forEach((level, index) => {
            this._buttons[index].setClass("outline", level !== current);
        });
    }
}

export class PinTable extends VBox {
    declare protected refs: { rows: VBox };

    private _settings: StateProxy<TwinSettings>;   // usedPinsOnly: which rows are shown
    private _rows: PinRow[] = [];                  // one per pin
    private _partOfPin: string[] = [];             // label of the part on each pin, "" if none
    private _last: PinState[] = [];                // the last state shown

    /**
     * An empty table, until setBoard(); 'settings' says which pins it shows.
     * The columns are resized from the header (x4js Header): it sets the
     * width of each as a CSS variable of the table, --pin-<column>-width,
     * that the cells of the rows take (main.scss).
     */
    constructor(props: BoxProps, settings: StateProxy<TwinSettings>) {
        super(props);
        this._settings = settings;

        // the columns: their variable, title, first width in px; the part
        // takes the room left (a negative width: flex, x4js)
        const columns = [
            { name: "pin-number", title: tx.twin.gpio, width: 48 },
            { name: "pin-level", title: "", width: 24 },
            { name: "pin-mode", title: tx.twin.mode, width: 160 },
            { name: "pin-pull", title: tx.twin.pull, width: 52 },
            { name: "pin-part", title: tx.twin.part, width: -1 },
            { name: "pin-force", title: tx.twin.force, width: 130 },
        ];

        // the header, then the rows
        this.setContent([
            new Header({ cls: "pin-header", target: this, items: columns }),
            this.refs.rows = new VBox({ cls: "rows" }),
        ]);

        // the setting changed (settings page): rows shown again
        settings.watch("usedPinsOnly", () => this._filter());
    }

    /**
     * Builds the rows of a board's pins (no part on them until setParts).
     */
    setBoard(board: BoardInfo) {
        this._partOfPin = new Array(board.pins).fill("");
        this._rows = this._partOfPin.map((_, pin) => new PinRow(pin));
        this.refs.rows.setContent(this._rows);
        this._last = [];
    }

    /**
     * The parts on each pin (their labels).
     */
    setParts(parts: PartInfo[]) {
        // the labels of the parts of each pin
        const labels: string[][] = this._partOfPin.map((): string[] => []);
        for (const part of parts) {
            for (const pin of part.pins) {
                labels[pin]?.push(part.label);
            }
        }

        // in the rows
        this._partOfPin = labels.map(list => list.join(", "));
        this._rows.forEach((row, pin) => row.setPart(this._partOfPin[pin]));
        this._filter();
    }

    /**
     * Shows the state of the pins.
     */
    setPins(pins: PinState[]) {
        pins.forEach((state, pin) => {
            this._rows[pin]?.update(state);
        });
        this._last = pins;
        this._filter();
    }

    /**
     * Hides the pins nothing uses, when the setting "used pins only" is on.
     */
    private _filter() {
        const usedOnly = this._settings.usedPinsOnly;
        this._rows.forEach((row, pin) => {
            const shown = !usedOnly || pinInUse(this._last[pin], this._partOfPin[pin] !== "");
            row.show(shown);
        });
    }
}

/**
 * Whether something uses a pin: the program configured it (gpio, rmt, i2c,
 * adc), the page forces it, or a part is on it ('hasPart'). The setting
 * "used pins only" shows only those (the table, the analyzer's list).
 */
export function pinInUse(state: PinState | undefined, hasPart: boolean): boolean {
    const usedByProgram = state !== undefined && (state.configured || state.rmt || state.i2c || state.adc);
    const forced = state !== undefined && state.forced !== null;
    return usedByProgram || forced || hasPart;
}
