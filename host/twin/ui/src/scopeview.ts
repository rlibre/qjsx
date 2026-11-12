/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The analyzer of the pins, a panel of the docking view (at the bottom): the
// level of the chosen pins over time, from the changes the twin records
// with their time (twin.scope.data, every 50 ms). A pin with an analog input
// that the program reads, or with a part on it, is drawn as its voltage.
// A pin that changed faster than the twin reports is drawn as a band
// between its two levels ("busy"). Each trace has its color (the first one
// free when the pin is added, changed by the menu of its button), on the
// black of a scope. Run scrolls with the time of the twin; Stop freezes the
// view, and Run starts a new acquisition. The pins chosen, their colors and
// the time base are kept by the browser.
import { BoxProps, Button, Canvas, CanvasEx, Flex, HBox, ListItem, MenuElement, Select, VBox } from "x4js";
import { BoardInfo, PartInfo, PinState, ScopeData } from "./messages";
import { tx } from "./texts";

// Name under which the browser keeps the pins chosen and the time base.
const SCOPE_KEY = "qjsx.twin.scope";

// The time bases offered, in µs per division (1, 2, 5 steps).
const TIME_BASES = [
    1, 2, 5, 10, 20, 50, 100, 200, 500,
    1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000,
    1000000,
];

const DIVISIONS = 10;                    // divisions across the view
const KEPT_US = 2 * DIVISIONS * 1000000; // history kept: twice the widest view
const LABEL_WIDTH = 120;                 // the column of the names of the pins, in px
const RIGHT_MARGIN = 8;                  // after the traces, in px
const AXIS_HEIGHT = 18;                  // the times under the traces, in px
const LOGIC_HEIGHT = 20;                 // height of a logic trace, in px
const ANALOG_HEIGHT = 60;                // height of an analog trace, in px
const ROW_GAP = 10;                      // between two traces, in px
const DEFAULT_VREF_MV = 3300;            // the level 1 of a pin, when the board does not say
const LABEL_STEPS = [1, 2, 5, 10];       // a time under every n divisions: the first n that leaves room
const LABEL_PADDING = 12;                // between two times, in px
const BUSY_OPACITY = 0.35;               // of a busy band, drawn in the color of its trace

// The colors of the traces: CSS variables --scope-color-<n> (main.scss),
// named in the menu of a pin (tx.twin.scope_colors).
const COLOR_COUNT = 8;

// The id of the first item of the "add a pin" list (not a pin).
const NO_PIN = "none";

// A pin shown, and the color of its trace.
interface Channel {
    pin:   number;
    color: number;                       // 0 .. COLOR_COUNT - 1
}

// What the browser keeps.
interface ScopeSettings {
    channels:  Channel[];                // the pins shown, in order
    perDivUs:  number;                   // the time base, in µs per division
}

// The state of a pin from 'time' on.
interface TracePoint {
    time:       number;                  // µs since the twin started
    level:      number;
    millivolts: number;
}

// A time when a pin changed faster than the twin reported.
interface BusyInterval {
    from: number;                        // µs
    to:   number;
}

// What a pin did: its states, oldest first, and its busy times.
interface PinTrace {
    points: TracePoint[];
    busy:   BusyInterval[];
}

// Where a pin is drawn (ScopeView._paintPin).
interface TraceRow {
    top:    number;                      // px
    height: number;                      // px
    analog: boolean;                     // drawn as its voltage
    color:  string;                      // of its trace
    start:  number;                      // the window of time, µs
    end:    number;
    left:   number;                      // the traces, px
    right:  number;
    xOf:    (time: number) => number;    // a time to its x
}

/**
 * A time in µs as a short text: "5 µs", "20 ms", "1 s".
 */
function formatTime(us: number): string {
    if (us === 0) {
        return "0";
    }
    if (Math.abs(us) < 1000) {
        return `${us} µs`;
    }
    if (Math.abs(us) < 1000000) {
        return `${us / 1000} ms`;
    }
    return `${us / 1000000} s`;
}

/**
 * The index of the first point after 'time' (points.length when none),
 * by bisection.
 */
function firstPointAfter(points: TracePoint[], time: number): number {
    let low = 0;
    let high = points.length;
    while (low < high) {
        const middle = (low + high) >> 1;
        if (points[middle].time <= time) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

export class ScopeView extends VBox {
    declare protected refs: { run: Button; timeBase: Select; addPin: Select; channels: HBox; canvas: Canvas };

    private _settings: ScopeSettings;        // kept by the browser
    private _traces: PinTrace[] = [];        // per pin; empty until the first message
    private _now = 0;                        // time of the last message, µs
    private _running = true;                 // Run: the view follows the time of the twin
    private _board: BoardInfo = null;
    private _pins: PinState[] = [];          // the state of the pins now (twin.state)
    private _parts: PartInfo[] = [];

    /**
     * An empty analyzer: its tool bar, then the traces.
     */
    constructor(props: BoxProps) {
        super(props);
        this._settings = this.loadPState(SCOPE_KEY, { channels: [], perDivUs: 1000 });

        // the time bases, the chosen one selected
        const timeBases: ListItem[] = TIME_BASES.map(us => ({ id: String(us), text: `${formatTime(us)}${tx.twin.scope_per_div}` }));

        // the tool bar, then the traces
        this.setContent([
            new HBox({ cls: "scope-bar", content: [
                this.refs.run = new Button({ cls: "outline", label: tx.twin.scope_stop, click: () => this._toggleRun() }),
                this.refs.timeBase = new Select({
                    items: timeBases,
                    value: String(this._settings.perDivUs),
                    change: () => this._setTimeBase(Number(this.refs.timeBase.getValue())),
                }),
                this.refs.addPin = new Select({ items: [], change: () => this._addChosenPin() }),
                this.refs.channels = new HBox({ cls: "scope-channels" }),
                new Flex(),
                new Button({ cls: "outline", label: tx.twin.scope_clear, click: () => this._clear() }),
            ] }),
            this.refs.canvas = new Canvas({ cls: "scope-canvas", flex: true, clear: true, paint_cb: ctx => this._paint(ctx) }),
        ]);

        this._showChannels();
    }

    /**
     * The board: its pins can be chosen.
     */
    setBoard(board: BoardInfo) {
        this._board = board;
        this._showChannels();
    }

    /**
     * The parts: their names go with the pins they are on.
     */
    setParts(parts: PartInfo[]) {
        this._parts = parts;
        this._showChannels();
    }

    /**
     * The state of the pins now: whether the program reads an analog input.
     */
    setPins(pins: PinState[]) {
        this._pins = pins;
    }

    /**
     * What the pins did since the last message (ignored while stopped: Run
     * starts a new acquisition).
     */
    addData(data: ScopeData) {
        if (!this._running) {
            return;
        }

        // the first message: each pin starts from its state now
        if (this._traces.length === 0) {
            this._traces = data.levels.map((level, pin): PinTrace => ({
                points: [{ time: data.now, level, millivolts: data.millivolts[pin] }],
                busy: [],
            }));
            this._now = data.now;
            return;
        }

        // the changes of each pin
        for (const change of data.changes) {
            const trace = this._traces[change.pin];
            if (trace === undefined) {
                continue;
            }

            const records = change.records;
            for (let i = 0; i < records.length; i += 3) {
                trace.points.push({ time: records[i], level: records[i + 1], millivolts: records[i + 2] });
            }

            // changes only counted: busy from the last record to the last of
            // them, then the state now
            if (change.dropped > 0) {
                const from = records.length > 0 ? records[records.length - 3] : this._now;
                trace.busy.push({ from, to: change.droppedUntil });
                trace.points.push({ time: change.droppedUntil, level: data.levels[change.pin], millivolts: data.millivolts[change.pin] });
            }
        }

        this._now = data.now;
        this._forgetOld();
        this.refs.canvas.redraw();
    }

    // ### Tool bar ###########################################################

    /**
     * Run <-> Stop. Back to Run: a new acquisition (the history is dropped).
     */
    private _toggleRun() {
        this._running = !this._running;
        this.refs.run.setText(this._running ? tx.twin.scope_stop : tx.twin.scope_run);
        if (this._running) {
            this._traces = [];
        }
        this.refs.canvas.redraw();
    }

    /**
     * A new time base.
     */
    private _setTimeBase(perDivUs: number) {
        this._settings.perDivUs = perDivUs;
        this.refs.canvas.redraw();
    }

    /**
     * The pin chosen in the "add a pin" list joins the traces, in the first
     * color no other trace has (the first one when all are taken).
     */
    private _addChosenPin() {
        const value = String(this.refs.addPin.getValue());
        if (value === NO_PIN) {
            return;
        }

        const taken = this._settings.channels.map(channel => channel.color);
        let color = 0;
        while (color < COLOR_COUNT && taken.includes(color)) {
            color++;
        }

        this._settings.channels.push({ pin: Number(value), color: color < COLOR_COUNT ? color : 0 });
        this._showChannels();
    }

    /**
     * A pin leaves the traces.
     */
    private _removeChannel(channel: Channel) {
        const index = this._settings.channels.indexOf(channel);
        if (index >= 0) {
            this._settings.channels.splice(index, 1);
            this._showChannels();
        }
    }

    /**
     * A new color for the trace of a pin.
     */
    private _setColor(channel: Channel, color: number) {
        channel.color = color;
        this._showChannels();
    }

    /**
     * The menu of the button of a pin: its colors (each marked with its
     * swatch, the current one checked), then remove.
     */
    private _channelMenu(channel: Channel): MenuElement[] {
        const colors: MenuElement[] = tx.twin.scope_colors.map((name, color) => ({
            text: name,
            cls: `scope-swatch scope-swatch-${color}`,
            checked: channel.color === color,
            click: () => this._setColor(channel, color),
        }));
        const remove: MenuElement = { text: tx.twin.scope_remove, click: () => this._removeChannel(channel) };
        return [...colors, "-", remove];
    }

    /**
     * The pins shown that the board has.
     */
    private _shownChannels(): Channel[] {
        const pinCount = this._board?.pins ?? 0;
        return this._settings.channels.filter(channel => channel.pin < pinCount);
    }

    /**
     * Forgets what was recorded; the next message starts again.
     */
    private _clear() {
        this._traces = [];
        this.refs.canvas.redraw();
    }

    /**
     * The tool bar for the pins chosen: a button per pin, marked with the
     * color of its trace (its menu: the colors, remove), the other pins in
     * the "add a pin" list; then the traces.
     */
    private _showChannels() {
        const pinCount = this._board?.pins ?? 0;
        const shown = this._shownChannels();

        // a button per pin shown
        this.refs.channels.setContent(shown.map(channel => new Button({
            cls: `outline scope-swatch scope-swatch-${channel.color}`,
            label: `GPIO${channel.pin}`,
            tooltip: tx.twin.scope_channel_tooltip,
            menu: () => this._channelMenu(channel),
        })));

        // the others to add
        const chosen = shown.map(channel => channel.pin);
        const items: ListItem[] = [{ id: NO_PIN, text: tx.twin.scope_add }];
        for (let pin = 0; pin < pinCount; pin++) {
            if (!chosen.includes(pin)) {
                items.push({ id: String(pin), text: this._pinName(pin) });
            }
        }
        this.refs.addPin.setItems(items);
        this.refs.addPin.setValue(NO_PIN);

        this.refs.canvas.redraw();
    }

    // ### History ############################################################

    /**
     * Drops what is older than KEPT_US (each pin keeps the point that gives
     * its state at the start of what is kept).
     */
    private _forgetOld() {
        const oldest = this._now - KEPT_US;
        for (const trace of this._traces) {
            const firstKept = firstPointAfter(trace.points, oldest) - 1;
            if (firstKept > 0) {
                trace.points.splice(0, firstKept);
            }
            trace.busy = trace.busy.filter(interval => interval.to >= oldest);
        }
    }

    // ### Drawing ############################################################

    /**
     * "GPIO5", with the parts on the pin: "GPIO5 · Relay".
     */
    private _pinName(pin: number): string {
        const labels = this._parts.filter(part => part.pins.includes(pin)).map(part => part.label);
        return labels.length > 0 ? `GPIO${pin} · ${labels.join(", ")}` : `GPIO${pin}`;
    }

    /**
     * Whether a pin is drawn as its voltage: it has an analog input, and the
     * program reads it or a part is on it.
     */
    private _isAnalog(pin: number): boolean {
        const analogInput = this._board?.adc?.pins.includes(pin) ?? false;
        const readByProgram = this._pins[pin]?.adc ?? false;
        const partOnIt = this._parts.some(part => part.pins.includes(pin));
        return analogInput && (readByProgram || partOnIt);
    }

    /**
     * The level 1 of a pin, in mV.
     */
    private _vrefMv(): number {
        const vref = this._board?.adc?.vref;
        return vref !== undefined ? vref * 1000 : DEFAULT_VREF_MV;
    }

    /**
     * Draws the view:
     * 1. the window of time: the last DIVISIONS divisions up to now (Run), or
     *    up to the last message received (Stop);
     * 2. the grid and the times under it (as many as there is room for);
     * 3. each pin chosen: its name, its busy bands, its trace, in its color.
     */
    private _paint(ctx: CanvasEx) {
        const canvas = this.refs.canvas;
        const style = canvas.getComputedStyle();
        const textColor = canvas.getStyleVariable("--scope-text");
        const gridColor = canvas.getStyleVariable("--scope-grid");
        ctx.font = `${style.fontSize} ${style.fontFamily}`;
        ctx.textBaseline = "middle";

        // the window of time, and where it goes
        const span = this._settings.perDivUs * DIVISIONS;
        const end = this._now;
        const start = end - span;
        const left = LABEL_WIDTH;
        const right = ctx.width - RIGHT_MARGIN;
        const bottom = ctx.height - AXIS_HEIGHT;
        const xOf = (time: number) => left + (time - start) * (right - left) / span;

        // the grid
        const divisionWidth = (right - left) / DIVISIONS;
        ctx.strokeStyle = gridColor;
        ctx.lineWidth = 1;
        for (let division = 0; division <= DIVISIONS; division++) {
            const x = Math.round(left + division * divisionWidth);
            ctx.beginPath();
            ctx.moveTo(x, 0);
            ctx.lineTo(x, bottom);
            ctx.stroke();
        }

        // the times under it (relative to the end), every 'step' divisions:
        // the first step whose room holds the widest time
        const widest = ctx.measureText(formatTime(-DIVISIONS * this._settings.perDivUs)).width + LABEL_PADDING;
        const step = LABEL_STEPS.find(candidate => candidate * divisionWidth >= widest) ?? DIVISIONS;
        ctx.fillStyle = textColor;
        for (let division = 0; division <= DIVISIONS; division += step) {
            const x = Math.round(left + division * divisionWidth);
            const last = division === DIVISIONS;
            ctx.textAlign = last ? "right" : "center";
            const offset = (division - DIVISIONS) * this._settings.perDivUs;
            ctx.fillText(formatTime(offset), x, bottom + AXIS_HEIGHT / 2);
        }

        // nothing chosen
        const shown = this._shownChannels();
        if (shown.length === 0) {
            ctx.textAlign = "center";
            ctx.fillText(tx.twin.scope_empty, (left + right) / 2, bottom / 2);
            return;
        }

        // each pin, one under the other, in its color
        let top = ROW_GAP;
        for (const channel of shown) {
            const analog = this._isAnalog(channel.pin);
            const height = analog ? ANALOG_HEIGHT : LOGIC_HEIGHT;
            const color = canvas.getStyleVariable(`--scope-color-${channel.color}`);
            this._paintPin(ctx, channel.pin, { top, height, analog, color, start, end, left, right, xOf }, textColor);
            top += height + ROW_GAP;
        }
    }

    /**
     * Draws a pin in its row: its name in the color of its trace (and its
     * scale when analog), its busy bands, then its trace, a step from each
     * state to the next.
     */
    private _paintPin(ctx: CanvasEx, pin: number, row: TraceRow, textColor: string) {
        const { top, height, analog, color, start, end, left, right, xOf } = row;
        const vrefMv = this._vrefMv();
        const yOf = (point: TracePoint) => analog
            ? top + height - point.millivolts * height / vrefMv
            : (point.level !== 0 ? top : top + height);

        // the name; an analog trace says its scale
        ctx.fillStyle = color;
        ctx.textAlign = "left";
        ctx.fillText(`GPIO${pin}`, 6, top + height / 2);
        if (analog) {
            ctx.fillStyle = textColor;
            ctx.textAlign = "right";
            ctx.fillText(`${vrefMv / 1000} V`, left - 6, top);
            ctx.fillText("0 V", left - 6, top + height);
        }

        const trace = this._traces[pin];
        if (trace === undefined || trace.points.length === 0) {
            return;
        }

        // only the window
        ctx.save();
        ctx.beginPath();
        ctx.rect(left, top - 2, right - left, height + 4);
        ctx.clip();

        // the busy bands, the color of the trace seen through
        ctx.fillStyle = color;
        ctx.globalAlpha = BUSY_OPACITY;
        for (const interval of trace.busy) {
            if (interval.to >= start && interval.from <= end) {
                const x = xOf(Math.max(interval.from, start));
                const width = Math.max(1, xOf(Math.min(interval.to, end)) - x);
                ctx.fillRect(x, top, width, height);
            }
        }
        ctx.globalAlpha = 1;

        // the trace: from the state at the start of the window (or the first
        // known state), a step at each change, up to the end
        const points = trace.points;
        const first = Math.max(0, firstPointAfter(points, start) - 1);
        let y = yOf(points[first]);
        ctx.strokeStyle = color;
        ctx.lineWidth = 1.5;
        ctx.beginPath();
        ctx.moveTo(Math.max(left, xOf(points[first].time)), y);
        for (let i = first + 1; i < points.length && points[i].time <= end; i++) {
            const x = xOf(points[i].time);
            ctx.lineTo(x, y);
            y = yOf(points[i]);
            ctx.lineTo(x, y);
        }
        ctx.lineTo(xOf(end), y);
        ctx.stroke();

        ctx.restore();
    }
}
