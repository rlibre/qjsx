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
// between its two levels ("busy"). A PWM output comes as its changes
// (frequency, duty cycle, from a time), an RMT send as it was sent (pulses,
// or bytes and the timing of their bits): their signal is drawn from them,
// at any zoom; too fast for the pixels, a band says what it is ("1 kHz ·
// 25 %", "RMT · 3 bytes"). Each trace has its color (the first one free
// when the pin is added), on the black of a scope; the menu of its button
// has a sub menu of the colors, one of the other pins (another pin in its
// place, same color), and remove.
//
// The tool bar, at the bottom, on two lines (user): the functions, then the
// pins. Run is a toggle: pressed, the view follows the time of the twin;
// released, the view freezes (on the last recording when there is one);
// pressed again, a new acquisition starts. Record (a toggle too) keeps
// everything from now on, the display untouched (no 20 s limit, up to
// MAX_CHANGES changes); released (or Run released), the recording is copied
// apart (scopetrace.ts: compact) and Run is released too: the view freezes
// on it (user). Both are filled while on (main.scss). A right click on the
// traces saves the recording as a VCD file (vcd.ts: PulseView, GTKWave) or
// clears (user).
//
// The modes (user, 2026-11-13): Roll, the view follows the twin; Normal, it
// stays on the last trigger (an edge of a chosen pin, at a tenth of the
// view from its left, the triggers a window apart at least: the holdoff of
// a scope); Single, armed when chosen or when Run is pressed, it freezes on
// the first trigger. The trigger is marked by a dashed line.
//
// The traces scroll up and down (the wheel) above the axis of the times,
// which stays; Ctrl + wheel zooms (the time bases of the list), around the
// pointer in a frozen view; Shift + wheel moves a frozen view in time
// (nothing while running, user). A tooltip on the traces says so. The
// times: from the trigger shown; else from now while running (0 at the
// right); a frozen view counts them from the start of its recording (as the
// VCD file), else from when it froze (user); the grid and the times move
// with the traces. An analog trace has a line at vref and one at 0 V,
// ticked and labelled at its left, in the small font of the scales (as the
// times). The pins chosen, their colors, the time base, the mode and the
// trigger are kept by the browser.
import { BoxProps, Button, Canvas, CanvasEx, Checkbox, HBox, Label, ListItem, Menu, MenuElement, Select, Space, StateProxy, VBox } from "x4js";
import { downloadText } from "./download";
import { frequencyText, percentText } from "./frequency";
import { BoardInfo, PartInfo, PinState, ScopeData, ScopePwm, ScopeRmt } from "./messages";
import { warn } from "./notify";
import { pinInUse } from "./pintable";
import {
    copyTraces, firstEdgeAfter, firstStateAfter, MAX_CHANGES, PinTrace, pwmChangeLevel, pwmChangeTime,
    pwmFirstChangeAfter, pwmLevelAt, pwmLevelBefore, PwmSegment, pwmToggles, Recording, rmtPulseCount,
    rmtPulseLevel, rmtPulseTicks, rmtTotalTicks, RmtTrain, StateList, TriggerEdge,
} from "./scopetrace";
import { TwinSettings } from "./settings";
import { tx } from "./texts";
import { recordingToVcd } from "./vcd";

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
const LOGIC_HEIGHT = 20;                 // height of a logic trace, in px
const ANALOG_HEIGHT = 60;                // height of an analog trace, in px
const ANALOG_MARGIN = 6;                 // above and below an analog trace, in px (its labels)
const ROW_GAP = 10;                      // between two traces, in px
const TICK_LENGTH = 4;                   // a tick of a scale, in px
const DEFAULT_VREF_MV = 3300;            // the level 1 of a pin, when the board does not say
const LABEL_STEPS = [1, 2, 5, 10];       // a time under every n divisions: the first n that leaves room
const LABEL_PADDING = 12;                // between two times, in px
const BUSY_OPACITY = 0.35;               // of a busy band, drawn in the color of its trace
const MIN_PERIOD_PX = 4;                 // a PWM period narrower than that: a band, not its square signal
const MIN_PULSE_PX = 2;                  // RMT pulses narrower than that on average: a band, not the pulses
const BAND_TEXT_PADDING = 8;             // around the text of a PWM band, in px
const EDGE_TOLERANCE = 1e-6;             // a division this close to an edge of the window (in divisions) is in it: rounding of the times

// The colors of the traces: CSS variables --scope-color-<n> (main.scss),
// named in the menu of a pin (tx.twin.scope_colors).
const COLOR_COUNT = 8;

// The id of the first item of the "add a pin" list (not a pin).
const NO_PIN = "none";

// The modes of the view: it follows the twin (roll), stays on the last
// trigger (normal), freezes on the first trigger (single).
type ScopeMode = "roll" | "normal" | "single";
const MODES: ScopeMode[] = ["roll", "normal", "single"];      // in the order of tx.twin.scope_modes
const EDGES: TriggerEdge[] = ["rising", "falling", "both"];   // in the order of tx.twin.scope_edges

const TRIGGER_SHARE = 0.1;               // where the trigger is in the view: a tenth from its left
const TRIGGER_GAP = 16;                  // px between the time base and the controls of the trigger, in the bar
const LOOKBACK_US = 100000;              // Normal looks for triggers that far back at most (two messages)

// A pin shown, and the color of its trace.
interface Channel {
    pin:   number;
    color: number;                       // 0 .. COLOR_COUNT - 1
}

// What the browser keeps.
interface ScopeSettings {
    channels:   Channel[];               // the pins shown, in order
    perDivUs:   number;                  // the time base, in µs per division
    mode:       ScopeMode;
    triggerPin: number;                  // the pin whose edges trigger; -1 (or a pin not shown): the first shown
    edge:       TriggerEdge;             // the edge that triggers
}

// The colors and fonts of the scales (ScopeView._paintPin).
interface ScaleLook {
    text:  string;                       // color of the labels
    grid:  string;                       // color of the lines
    fonts: { normal: string; small: string };   // CSS fonts: the page's, the scales'
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

// The window of time the view shows, and where the traces go.
interface ViewWindow {
    start: number;                       // µs
    end:   number;
    left:  number;                       // px: the traces, after the names
    right: number;
}

// A division of the grid in a window (ScopeView._divisions).
interface Division {
    index: number;                       // divisions from the zero of the times (negative before it)
    time:  number;                       // µs, the time of the twin
}

/**
 * Where a time goes in a window, in px.
 */
function xOfTime(view: ViewWindow, time: number) {
    return view.left + (time - view.start) * (view.right - view.left) / (view.end - view.start);
}

/**
 * The y of a level in a row: high at its top (vref in an analog row), low
 * at its bottom.
 */
function levelY(row: TraceRow, level: number) {
    return level !== 0 ? row.top : row.top + row.height;
}

/**
 * Ends the running PWM segment of a trace at 'time' (not before its start).
 */
function endPwm(trace: PinTrace, time: number) {
    const last = trace.pwm[trace.pwm.length - 1];
    if (last !== undefined && last.to === Infinity) {
        last.to = Math.max(last.from, time);
    }
}

/**
 * The segment of a PWM output running now, from its first period.
 */
function segmentOf(running: ScopePwm): PwmSegment {
    return { from: running.since, to: Infinity, frequency: running.frequency, duty: running.duty, invert: running.invert };
}

/**
 * The train of an RMT send, its end computed from its pulses.
 */
function trainOf(send: ScopeRmt): RmtTrain {
    const train: RmtTrain = {
        from: send.time,
        to: send.time,
        tickUs: 1000000 / send.resolution,
        pulses: send.pulses ?? [],
        bytes: send.bytes ?? [],
        bit0: send.bit0 ?? [0, 0],
        bit1: send.bit1 ?? [0, 0],
        reset: send.reset ?? 0,
    };
    train.to = train.from + rmtTotalTicks(train) * train.tickUs;
    return train;
}

/**
 * The name of a VCD file of a recording: "qjsx-analyzer-20261113-143205.vcd".
 */
function recordingFileName() {
    const now = new Date();
    const two = (value: number) => String(value).padStart(2, "0");
    const day = `${now.getFullYear()}${two(now.getMonth() + 1)}${two(now.getDate())}`;
    const time = `${two(now.getHours())}${two(now.getMinutes())}${two(now.getSeconds())}`;
    return `qjsx-analyzer-${day}-${time}.vcd`;
}

export class ScopeView extends VBox {
    declare protected refs: {
        run: Checkbox;
        record: Checkbox;
        timeBase: Select;
        mode: Select;
        trigger: Select;                 // the pin that triggers
        edge: Select;
        status: Label;                   // "waiting for a trigger", or nothing
        addPin: Select;
        channels: HBox;
        scroller: VBox;                  // the traces scroll in it
        canvas: Canvas;                  // the traces, as high as they need
        axis: Canvas;                    // the times, under the traces
    };

    private _settings: ScopeSettings;        // kept by the browser
    private _twinSettings: StateProxy<TwinSettings>;   // usedPinsOnly: which pins the "add a pin" list offers
    private _offered = "";                   // the pins the "add a pin" list offers, as a key ("4,5,6")
    private _traces: PinTrace[] = [];        // per pin; empty until the first message
    private _now = 0;                        // time of the last message, µs
    private _running = true;                 // Run: the view follows the time of the twin
    private _viewEnd = 0;                    // stopped: the end of the view, µs
    private _stoppedAt = 0;                  // stopped: when the view froze, µs (the zero of its times without a recording)
    private _recordStart: number = null;     // recording: when it started (the first message), µs; null: not recording
    private _recordAsked = false;            // Record is on (the recording starts at the next message)
    private _recorded = 0;                   // changes in the recording
    private _recording: Recording = null;    // the last recording, for Save; null: none
    private _trigger: number = null;         // the trigger shown (Normal) or caught (Single), µs; null: none yet
    private _armedAt = 0;                    // Single: when it was armed, µs (its trigger comes after)
    private _waitingEnd = 0;                 // waiting for a trigger: the end of the view, µs (it stays there)
    private _board: BoardInfo = null;
    private _pins: PinState[] = [];          // the state of the pins now (twin.state)
    private _parts: PartInfo[] = [];

    /**
     * An empty analyzer: its tool bar, then the traces; 'twinSettings' says
     * which pins its "add a pin" list offers.
     */
    constructor(props: BoxProps, twinSettings: StateProxy<TwinSettings>) {
        super(props);
        const defaults: ScopeSettings = { channels: [], perDivUs: 1000, mode: "roll", triggerPin: -1, edge: "rising" };
        this._settings = this.loadPState(SCOPE_KEY, defaults);
        this._twinSettings = twinSettings;

        // the lists: the time bases, the modes, the edges (the chosen ones
        // selected)
        const timeBases: ListItem[] = TIME_BASES.map(us => ({ id: String(us), text: `${formatTime(us)}${tx.twin.scope_per_div}` }));
        const modes: ListItem[] = MODES.map((mode, index) => ({ id: mode, text: tx.twin.scope_modes[index] }));
        const edges: ListItem[] = EDGES.map((edge, index) => ({ id: edge, text: tx.twin.scope_edges[index] }));

        // the traces, the times under them, then the tool bar (at the
        // bottom, as on a scope) on two lines (user): its functions, then
        // the pins
        this.setContent([
            this.refs.scroller = new VBox({ cls: "scope-scroller", flex: true, tooltip: tx.twin.scope_traces_tooltip, content: [
                this.refs.canvas = new Canvas({ cls: "scope-canvas", clear: true, paint_cb: ctx => this._paint(ctx) }),
            ] }),
            this.refs.axis = new Canvas({ cls: "scope-axis", clear: true, paint_cb: ctx => this._paintAxis(ctx) }),
            new VBox({ cls: "scope-bar", content: [
                new HBox({ cls: "scope-bar-line", content: [
                    // pressed while the view follows the twin (an acquisition runs)
                    this.refs.run = new Checkbox({
                        cls: "button scope-run",
                        label: tx.twin.scope_run,
                        tooltip: tx.twin.scope_run_tooltip,
                        checked: true,
                        change: () => this._toggleRun(),
                    }),
                    // pressed while recording, in the color of the alerts (x4js: "danger")
                    this.refs.record = new Checkbox({
                        cls: "button danger",
                        label: tx.twin.scope_record,
                        tooltip: tx.twin.scope_record_tooltip,
                        change: () => this._setRecord(this.refs.record.getCheck()),
                    }),
                    this.refs.timeBase = new Select({
                        items: timeBases,
                        value: String(this._settings.perDivUs),
                        change: () => this._setTimeBase(Number(this.refs.timeBase.getValue())),
                    }),
                    // apart (user): the mode, and what triggers
                    new Space(TRIGGER_GAP),
                    this.refs.mode = new Select({
                        items: modes,
                        value: this._settings.mode,
                        tooltip: tx.twin.scope_mode_tooltip,
                        change: () => this._setMode(this.refs.mode.getValue() as ScopeMode),
                    }),
                    this.refs.trigger = new Select({
                        items: [],
                        tooltip: tx.twin.scope_trigger_tooltip,
                        change: () => this._setTriggerPin(Number(this.refs.trigger.getValue())),
                    }),
                    this.refs.edge = new Select({
                        items: edges,
                        value: this._settings.edge,
                        tooltip: tx.twin.scope_edge_tooltip,
                        change: () => this._setEdge(this.refs.edge.getValue() as TriggerEdge),
                    }),
                    this.refs.status = new Label({ cls: "scope-status", text: "" }),
                ] }),
                new HBox({ cls: "scope-bar-line", content: [
                    this.refs.addPin = new Select({ items: [], change: () => this._addChosenPin() }),
                    this.refs.channels = new HBox({ cls: "scope-channels" }),
                ] }),
            ] }),
        ]);

        // the traces as high as they need, at least the room they have
        this.refs.scroller.addDOMEvent("resized", () => this._redraw());

        // the wheel: alone it scrolls the traces, Shift moves in time, Ctrl
        // zooms
        this.refs.scroller.addDOMEvent("wheel", (event: WheelEvent) => this._onWheel(event));

        // a right click on the traces: save, clear (user)
        this.refs.scroller.addDOMEvent("contextmenu", (event: MouseEvent) => this._showMenu(event));

        this._showChannels();

        // the setting changed (settings page): the list offers again
        twinSettings.watch("usedPinsOnly", () => this._fillAddList());
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
     * The state of the pins now: whether the program reads an analog input,
     * which pins are used (the "add a pin" list).
     */
    setPins(pins: PinState[]) {
        this._pins = pins;
        this._fillAddList();
    }

    /**
     * What the pins did since the last message (ignored while stopped: Run
     * starts a new acquisition).
     */
    addData(data: ScopeData) {
        if (!this._running) {
            return;
        }

        // the first message: each pin starts from its state now, a PWM
        // running from its first period; a recording asked starts there
        if (this._traces.length === 0) {
            this._traces = data.levels.map((level, pin): PinTrace => {
                const states = new StateList();
                states.push(data.now, level, data.millivolts[pin]);
                return { states, busy: [], pwm: [], rmt: [] };
            });
            for (const running of data.pwm) {
                this._traces[running.pin]?.pwm.push(segmentOf(running));
            }
            this._now = data.now;

            // a trigger looked for from there (Single armed there), the
            // view waiting there
            this._armedAt = data.now;
            this._waitingEnd = data.now;
            if (this._recordAsked) {
                this._recordStart = data.now;
                this._recorded = 0;
            }
            return;
        }

        // the changes of each pin
        for (const change of data.changes) {
            const trace = this._traces[change.pin];
            if (trace === undefined) {
                continue;
            }

            // its changes of level
            const records = change.records;
            for (let i = 0; i < records.length; i += 3) {
                trace.states.push(records[i], records[i + 1], records[i + 2]);
            }

            // those of its PWM: the running segment ends, a frequency starts
            // another
            const pwm = change.pwm;
            for (let i = 0; i < pwm.length; i += 4) {
                const time = pwm[i];
                const frequency = pwm[i + 1];
                endPwm(trace, time);
                if (frequency > 0) {
                    trace.pwm.push({ from: time, to: Infinity, frequency, duty: pwm[i + 2], invert: pwm[i + 3] !== 0 });
                }
            }
            this._recorded += records.length / 3 + pwm.length / 4;

            // changes only counted: busy from the last record to the last of
            // them, then the state now, and the PWM now
            if (change.dropped > 0) {
                const from = records.length > 0 ? records[records.length - 3] : this._now;
                trace.busy.push({ from, to: change.droppedUntil });
                trace.states.push(change.droppedUntil, data.levels[change.pin], data.millivolts[change.pin]);

                // the PWM now, unless it is the segment running (the time of
                // its setting tells it)
                const running = data.pwm.find(item => item.pin === change.pin);
                const last = trace.pwm[trace.pwm.length - 1];
                const unchanged = running !== undefined && last?.to === Infinity && last.from === running.since;
                if (!unchanged) {
                    endPwm(trace, running?.since ?? change.droppedUntil);
                    if (running !== undefined) {
                        trace.pwm.push(segmentOf(running));
                    }
                }
            }
        }

        // the RMT sends, on their pin
        for (const send of data.rmt) {
            this._traces[send.pin]?.rmt.push(trainOf(send));
        }
        this._recorded += data.rmt.length;

        this._now = data.now;

        // a recording keeps everything, up to MAX_CHANGES changes; else
        // what is too old goes
        if (this._recordStart !== null && this._recorded >= MAX_CHANGES) {
            warn(tx.twin.scope_record, tx.twin.scope_record_full);
            this._setRecord(false);
        } else if (this._recordStart === null) {
            this._forgetOld();
        }

        // the trigger in what came (Normal, Single)
        this._findTrigger();
        this._redraw();
    }

    // ### Tool bar ###########################################################

    /**
     * Draws the traces and the times again; the traces first take the height
     * they need (the scroller scrolls past its room). The bar says when the
     * view waits for a trigger.
     */
    private _redraw() {
        const needed = this._tracesHeight();
        const room = this.refs.scroller.dom.clientHeight;
        this.refs.canvas.setStyleValue("height", Math.max(needed, room));
        this.refs.canvas.redraw();
        this.refs.axis.redraw();
        this.refs.status.setText(this._waiting() ? tx.twin.scope_waiting : "");
    }

    /**
     * The height of the traces: each pin's row and the gaps (an analog row
     * with its margins).
     */
    private _tracesHeight() {
        let height = ROW_GAP;
        for (const channel of this._shownChannels()) {
            const rowHeight = this._isAnalog(channel.pin) ? ANALOG_HEIGHT + 2 * ANALOG_MARGIN : LOGIC_HEIGHT;
            height += rowHeight + ROW_GAP;
        }
        return height;
    }

    /**
     * Run pressed or released. Released: the recording on ends, the view
     * freezes; pressed again: a new acquisition (the history is dropped, the
     * last recording kept for Save).
     */
    private _toggleRun() {
        if (!this._running) {
            this._start();
            return;
        }
        if (this._recordAsked) {
            this._setRecord(false);
        } else {
            this._stop();
        }
    }

    /**
     * Run: a new acquisition, the view following the time of the twin (or
     * waiting for a trigger: Single is armed at its first message).
     */
    private _start() {
        this._running = true;
        this._traces = [];
        this._trigger = null;
        this.refs.run.setCheck(true);
        this._redraw();
    }

    /**
     * Stop: the view frozen, on the end of the last recording when there is
     * one, else where it is.
     */
    private _stop() {
        const end = this._liveEnd();
        this._running = false;
        this._stoppedAt = this._now;
        this._viewEnd = this._recording !== null ? this._recording.end : end;
        this.refs.run.setCheck(false);
        this._redraw();
    }

    /**
     * Record on: everything is kept from now on, the display untouched (the
     * view stopped: a new acquisition starts, the recording with it); the
     * last recording is dropped. Off: what was recorded is copied apart, for
     * Save, and Run is released: the view freezes on the recording.
     */
    private _setRecord(on: boolean) {
        this.refs.record.setCheck(on);
        this._recordAsked = on;
        if (on) {
            this._recording = null;
            this._recorded = 0;
            if (!this._running) {
                this._recordStart = null;
                this._start();
            } else {
                this._recordStart = this._traces.length > 0 ? this._now : null;
            }
            return;
        }

        // the end of a recording that started: a copy of what it holds
        if (this._recordStart !== null) {
            const traces = copyTraces(this._traces, this._recordStart, this._now);
            this._recording = { traces, start: this._recordStart, end: this._now };
        }
        this._recordStart = null;
        this._stop();
    }

    /**
     * Gives the last recording, the pins shown, as a VCD file; says so when
     * the file had to stop at MAX_CHANGES edges (a fast PWM).
     */
    private _save() {
        if (this._recording === null) {
            return;
        }
        const channels = this._shownChannels().map(channel => ({
            pin: channel.pin,
            name: `GPIO${channel.pin}`,
            analog: this._isAnalog(channel.pin),
            vref: this._vrefMv() / 1000,
        }));
        const vcd = recordingToVcd(this._recording, channels);
        downloadText(recordingFileName(), vcd.parts);
        if (vcd.cutAt !== null) {
            warn(tx.twin.scope_save, `${tx.twin.scope_save_cut} ${formatTime(Math.round(vcd.cutAt))}`);
        }
    }

    /**
     * The menu of a right click on the traces: save the last recording (when
     * there is one), clear. Its own: the menu of the panels (twinview.ts)
     * does not open too.
     */
    private _showMenu(event: MouseEvent) {
        event.preventDefault();
        event.stopPropagation();
        const menu = new Menu({
            items: [
                { text: tx.twin.scope_save, disabled: this._recording === null, click: () => this._save() },
                { text: tx.twin.scope_clear, click: () => this._clear() },
            ],
        });
        menu.displayAt(event.clientX, event.clientY);
    }

    /**
     * A new time base.
     */
    private _setTimeBase(perDivUs: number) {
        this._settings.perDivUs = perDivUs;
        this._redraw();
    }

    // ### Trigger ############################################################

    /**
     * Another mode: the trigger looked for again (Single armed now, Run
     * pressed if it was not).
     */
    private _setMode(mode: ScopeMode) {
        this._settings.mode = mode;
        this._rearm();
    }

    /**
     * Another pin triggers: looked for again.
     */
    private _setTriggerPin(pin: number) {
        this._settings.triggerPin = pin;
        this._rearm();
    }

    /**
     * Another edge triggers: looked for again.
     */
    private _setEdge(edge: TriggerEdge) {
        this._settings.edge = edge;
        this._rearm();
    }

    /**
     * Forgets the trigger shown: the view waits where it is for the next one;
     * Single is armed from now (a new acquisition when the view was frozen).
     */
    private _rearm() {
        this._trigger = null;
        this._waitingEnd = this._now;
        this._armedAt = this._now;
        if (this._settings.mode === "single" && !this._running) {
            this._start();
            return;
        }
        this._redraw();
    }

    /**
     * The pin that triggers: the one chosen when it is shown, else the first
     * shown; -1 when none is shown.
     */
    private _triggerPin(): number {
        const shown = this._shownChannels().map(channel => channel.pin);
        if (shown.includes(this._settings.triggerPin)) {
            return this._settings.triggerPin;
        }
        return shown.length > 0 ? shown[0] : -1;
    }

    /**
     * The list of the pins that may trigger: those shown, the one that
     * triggers selected.
     */
    private _fillTriggerList() {
        const items: ListItem[] = this._shownChannels().map(channel => ({ id: String(channel.pin), text: `GPIO${channel.pin}` }));
        this.refs.trigger.setItems(items);
        this.refs.trigger.setValue(String(this._triggerPin()));
    }

    /**
     * Whether the view waits for a trigger (Normal or Single running, none
     * found yet).
     */
    private _waiting() {
        return this._running && this._settings.mode !== "roll" && this._trigger === null;
    }

    /**
     * Whether the view shows a trigger (Normal or Single, one found).
     */
    private _triggerShown() {
        return this._settings.mode !== "roll" && this._trigger !== null;
    }

    /**
     * Looks for the trigger in what came (nothing in Roll, nor frozen):
     * - Normal: the triggers follow one another at least a window apart
     *   (the holdoff, as a scope), from the last one shown (or from the
     *   recent past); the view takes the newest whose window is complete
     *   (what comes after it in the view has come);
     * - Single: the first trigger after it was armed, once its window is
     *   complete: the view freezes on it (Run released).
     */
    private _findTrigger() {
        // a mode that triggers, a pin, running
        const mode = this._settings.mode;
        const pin = this._triggerPin();
        const trace = this._traces[pin];
        if (mode === "roll" || trace === undefined || !this._running) {
            return;
        }

        // what a trigger needs: its window after it complete
        const span = this._settings.perDivUs * DIVISIONS;
        const afterUs = span * (1 - TRIGGER_SHARE);
        const edge = this._settings.edge;
        const complete = (time: number) => time + afterUs <= this._now;

        // Single: the first one after it was armed, then frozen on it
        if (mode === "single") {
            const caught = firstEdgeAfter(trace, edge, this._armedAt);
            if (caught !== null && complete(caught)) {
                this._trigger = caught;
                this._freezeOnTrigger();
            }
            return;
        }

        // Normal: from the holdoff of the last one, each next one while its
        // window is complete
        const recent = this._now - span - LOOKBACK_US;
        let from = this._trigger !== null ? Math.max(recent, this._trigger + span) : recent;
        for (;;) {
            const next = firstEdgeAfter(trace, edge, from);
            if (next === null || !complete(next)) {
                break;
            }
            this._trigger = next;
            from = next + span;
        }
    }

    /**
     * Single caught its trigger: the acquisition stops (a recording on ends
     * with it), the view on the trigger.
     */
    private _freezeOnTrigger() {
        if (this._recordAsked) {
            this._setRecord(false);
        } else {
            this._stop();
        }

        const span = this._settings.perDivUs * DIVISIONS;
        this._viewEnd = this._trigger + span * (1 - TRIGGER_SHARE);
        this._redraw();
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
     * Another pin in the place of a trace, with its color: the history of
     * every pin is kept, so the new one is drawn at once.
     */
    private _switchPin(channel: Channel, pin: number) {
        channel.pin = pin;
        this._showChannels();
    }

    /**
     * The menu of the button of a pin: a sub menu of the colors (each marked
     * with its swatch, the current one checked), a sub menu of the pins it
     * can switch to (those the "add a pin" list offers), then remove.
     */
    private _channelMenu(channel: Channel): MenuElement[] {
        // the colors
        const colors: MenuElement[] = tx.twin.scope_colors.map((name, color) => ({
            text: name,
            cls: `scope-swatch scope-swatch-${color}`,
            checked: channel.color === color,
            click: () => this._setColor(channel, color),
        }));

        // the other pins
        const pins: MenuElement[] = this._offeredPins().map(pin => ({
            text: this._pinName(pin),
            click: () => this._switchPin(channel, pin),
        }));

        // a sub menu each, then remove
        return [
            { text: tx.twin.scope_color, menu: new Menu({ items: colors }) },
            { text: tx.twin.scope_switch_pin, menu: new Menu({ items: pins }), disabled: pins.length === 0 },
            "-",
            { text: tx.twin.scope_remove, click: () => this._removeChannel(channel) },
        ];
    }

    /**
     * The pins shown that the board has.
     */
    private _shownChannels(): Channel[] {
        const pinCount = this._board?.pins ?? 0;
        return this._settings.channels.filter(channel => channel.pin < pinCount);
    }

    /**
     * Forgets what was acquired and the last recording; the next message
     * starts again (a recording on starts again too).
     */
    private _clear() {
        this._traces = [];
        this._recordStart = null;
        this._recording = null;
        this._trigger = null;
        this._redraw();
    }

    // ### Moving a frozen view ###############################################

    /**
     * The window of time shown, in a view 'width' px wide: running, as the
     * mode places it (_liveEnd); frozen, up to where the frozen view is.
     */
    private _window(width: number): ViewWindow {
        const end = this._running ? this._liveEnd() : this._viewEnd;
        const start = end - this._settings.perDivUs * DIVISIONS;
        return { start, end, left: LABEL_WIDTH, right: width - RIGHT_MARGIN };
    }

    /**
     * Where the view ends while running: now (Roll); the trigger shown at a
     * tenth of the view from its left (Normal, Single); where it was while
     * it waits for one.
     */
    private _liveEnd() {
        if (this._settings.mode === "roll") {
            return this._now;
        }
        if (this._trigger === null) {
            return this._waitingEnd;
        }
        const span = this._settings.perDivUs * DIVISIONS;
        return this._trigger + span * (1 - TRIGGER_SHARE);
    }

    /**
     * The time the axis shows as 0, µs: the trigger shown (Normal, Single,
     * running or frozen); the end of the view while it waits for one; now
     * in Roll (0 at the right); frozen, the start of the recording shown (the
     * times of its VCD file), else when the view froze.
     */
    private _timeZero() {
        if (this._triggerShown()) {
            return this._trigger;
        }
        if (this._waiting()) {
            return this._waitingEnd;
        }
        if (this._running) {
            return this._now;
        }
        return this._frozenOnRecording() ? this._recording.start : this._stoppedAt;
    }

    /**
     * The divisions of the grid in a window: the multiples of the time base
     * counted from the zero of the times, so the grid and the times move
     * with the traces when a frozen view moves (running, they fall on the
     * same places: the window ends at the zero).
     */
    private _divisions(view: ViewWindow): Division[] {
        // the first and last multiples in the window
        const perDiv = this._settings.perDivUs;
        const zero = this._timeZero();
        const first = Math.ceil((view.start - zero) / perDiv - EDGE_TOLERANCE);
        const last = Math.floor((view.end - zero) / perDiv + EDGE_TOLERANCE);

        // each one, with its time
        const divisions: Division[] = [];
        for (let index = first; index <= last; index++) {
            divisions.push({ index, time: zero + index * perDiv });
        }
        return divisions;
    }

    /**
     * The wheel over the traces: alone, the browser scrolls them up and
     * down; Shift: the view moves in time; Ctrl: the next time base.
     */
    private _onWheel(event: WheelEvent) {
        if (event.shiftKey) {
            event.preventDefault();
            this._moveInTime(event);
        } else if (event.ctrlKey) {
            event.preventDefault();
            this._zoom(event);
        }
    }

    /**
     * Shift + wheel: the view moves in time, as many pixels as the wheel
     * turned (down / right: later); nothing while running (user). (Shift
     * turns the wheel horizontal in some browsers: deltaX then.)
     */
    private _moveInTime(event: WheelEvent) {
        if (this._running) {
            return;
        }
        const delta = event.deltaX !== 0 ? event.deltaX : event.deltaY;
        const view = this._window(this.refs.canvas.dom.clientWidth);
        const usPerPixel = (view.end - view.start) / (view.right - view.left);
        this._viewEnd += delta * usPerPixel;
        this._redraw();
    }

    /**
     * Ctrl + wheel: the next time base (down: wider, up: closer); a frozen
     * view keeps the time under the pointer where it is.
     */
    private _zoom(event: WheelEvent) {

        // the next time base
        const index = TIME_BASES.indexOf(this._settings.perDivUs);
        const next = Math.min(TIME_BASES.length - 1, Math.max(0, index + (event.deltaY > 0 ? 1 : -1)));
        if (next === index || index < 0) {
            return;
        }

        // the time under the pointer stays there
        if (!this._running) {
            const view = this._window(this.refs.canvas.dom.clientWidth);
            const pointerX = event.clientX - this.refs.canvas.getBoundingRect().left;
            const share = Math.min(1, Math.max(0, (pointerX - view.left) / (view.right - view.left)));
            const pointed = view.start + share * (view.end - view.start);
            const span = TIME_BASES[next] * DIVISIONS;
            this._viewEnd = pointed + (1 - share) * span;
        }

        this.refs.timeBase.setValue(String(TIME_BASES[next]));
        this._setTimeBase(TIME_BASES[next]);
    }

    /**
     * The tool bar for the pins chosen: a button per pin, marked with the
     * color of its trace (its menu: the colors, switch pin, remove), the
     * other pins in the "add a pin" list; then the traces.
     */
    private _showChannels() {
        // a button per pin shown
        this.refs.channels.setContent(this._shownChannels().map(channel => new Button({
            cls: `outline scope-swatch scope-swatch-${channel.color}`,
            label: `GPIO${channel.pin}`,
            tooltip: tx.twin.scope_channel_tooltip,
            menu: () => this._channelMenu(channel),
        })));

        // the others to add, offered again; the pins that may trigger
        this._offered = "";
        this._fillAddList();
        this._fillTriggerList();

        this._redraw();
    }

    /**
     * The pins not shown yet, only those in use when the setting "used pins
     * only" is on: the "add a pin" list and the "switch pin" sub menu.
     */
    private _offeredPins(): number[] {
        const pinCount = this._board?.pins ?? 0;
        const chosen = this._shownChannels().map(channel => channel.pin);
        const usedOnly = this._twinSettings.usedPinsOnly;
        const offered: number[] = [];
        for (let pin = 0; pin < pinCount; pin++) {
            const hasPart = this._parts.some(part => part.pins.includes(pin));
            const allowed = !usedOnly || pinInUse(this._pins[pin], hasPart);
            if (!chosen.includes(pin) && allowed) {
                offered.push(pin);
            }
        }
        return offered;
    }

    /**
     * The "add a pin" list: the pins offered (_offeredPins). Filled again only
     * when that set changes (the state of the pins comes every 50 ms or so).
     */
    private _fillAddList() {
        // the pins to offer
        const offered = this._offeredPins();

        // the same as now: nothing to do
        const key = offered.join(",");
        if (key === this._offered) {
            return;
        }
        this._offered = key;

        // the list, its first item selected
        const items: ListItem[] = [{ id: NO_PIN, text: tx.twin.scope_add }];
        for (const pin of offered) {
            items.push({ id: String(pin), text: this._pinName(pin) });
        }
        this.refs.addPin.setItems(items);
        this.refs.addPin.setValue(NO_PIN);
    }

    // ### History ############################################################

    /**
     * What the view draws: the last recording when the view is frozen and
     * there is one, else what is acquired.
     */
    private _shownTraces(): PinTrace[] {
        return this._frozenOnRecording() ? this._recording.traces : this._traces;
    }

    /**
     * The view is frozen and shows the last recording.
     */
    private _frozenOnRecording() {
        return !this._running && this._recording !== null;
    }

    /**
     * Drops what is older than KEPT_US (each pin keeps the point that gives
     * its state at the start of what is kept).
     */
    private _forgetOld() {
        const oldest = this._now - KEPT_US;
        for (const trace of this._traces) {
            const firstKept = firstStateAfter(trace.states, oldest) - 1;
            if (firstKept > 0) {
                trace.states.dropOldest(firstKept);
            }
            trace.busy = trace.busy.filter(interval => interval.to >= oldest);
            trace.pwm = trace.pwm.filter(segment => segment.to >= oldest);
            trace.rmt = trace.rmt.filter(train => train.to >= oldest);
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
     * The fonts of the drawing: the page's, and the small one of the scales
     * (the times, the volts).
     */
    private _fonts() {
        const canvas = this.refs.canvas;
        const style = canvas.getComputedStyle();
        const small = canvas.getStyleVariable("--scope-scale-font-size");
        return { normal: `${style.fontSize} ${style.fontFamily}`, small: `${small} ${style.fontFamily}` };
    }

    /**
     * Draws the traces:
     * 1. the window of time: the last DIVISIONS divisions up to now (Run), or
     *    up to where the frozen view is;
     * 2. the lines of the divisions, the whole height, at their times (they
     *    move with the traces: _divisions); the trigger shown, dashed;
     * 3. each pin chosen: its name, its scale when analog, its busy bands,
     *    its trace, in its color.
     */
    private _paint(ctx: CanvasEx) {
        const canvas = this.refs.canvas;
        const textColor = canvas.getStyleVariable("--scope-text");
        const gridColor = canvas.getStyleVariable("--scope-grid");
        const fonts = this._fonts();
        ctx.font = fonts.normal;
        ctx.textBaseline = "middle";

        // 1. the window of time, and where it goes
        const view = this._window(ctx.width);
        const { start, end, left, right } = view;
        const xOf = (time: number) => xOfTime(view, time);

        // 2. the lines of the divisions
        ctx.strokeStyle = gridColor;
        ctx.lineWidth = 1;
        for (const division of this._divisions(view)) {
            const x = Math.round(xOf(division.time));
            ctx.beginPath();
            ctx.moveTo(x, 0);
            ctx.lineTo(x, ctx.height);
            ctx.stroke();
        }

        // the trigger shown: a dashed line, the whole height
        if (this._triggerShown() && this._trigger >= start && this._trigger <= end) {
            const x = Math.round(xOf(this._trigger));
            ctx.strokeStyle = textColor;
            ctx.setLineDash([4, 4]);
            ctx.beginPath();
            ctx.moveTo(x, 0);
            ctx.lineTo(x, ctx.height);
            ctx.stroke();
            ctx.setLineDash([]);
        }

        // nothing chosen
        const shown = this._shownChannels();
        if (shown.length === 0) {
            ctx.fillStyle = textColor;
            ctx.textAlign = "center";
            ctx.fillText(tx.twin.scope_empty, (left + right) / 2, this.refs.scroller.dom.clientHeight / 2);
            return;
        }

        // 3. each pin, one under the other, in its color (an analog one
        // between its margins)
        let top = ROW_GAP;
        for (const channel of shown) {
            const analog = this._isAnalog(channel.pin);
            const height = analog ? ANALOG_HEIGHT : LOGIC_HEIGHT;
            const margin = analog ? ANALOG_MARGIN : 0;
            const color = canvas.getStyleVariable(`--scope-color-${channel.color}`);
            const row = { top: top + margin, height, analog, color, start, end, left, right, xOf };
            this._paintPin(ctx, channel.pin, row, { text: textColor, grid: gridColor, fonts });
            top += height + 2 * margin + ROW_GAP;
        }
    }

    /**
     * Draws the times under the traces, from the zero of the times
     * (_timeZero), in the small font: a tick at each division, a time every
     * 'step' of them counted from the zero, so a time stays on its division
     * when the view moves ('step': the first one whose room holds the widest
     * time). The traces' width gives the places (a scroll bar may take some
     * of it).
     */
    private _paintAxis(ctx: CanvasEx) {
        const canvas = this.refs.canvas;
        const view = this._window(canvas.dom.clientWidth);
        const divisions = this._divisions(view);
        ctx.font = this._fonts().small;
        ctx.textBaseline = "middle";

        // the ticks
        ctx.strokeStyle = canvas.getStyleVariable("--scope-grid");
        ctx.lineWidth = 1;
        for (const division of divisions) {
            const x = Math.round(xOfTime(view, division.time));
            ctx.beginPath();
            ctx.moveTo(x, 0);
            ctx.lineTo(x, TICK_LENGTH);
            ctx.stroke();
        }

        // the texts (from the index: no rounding of the times), and how
        // many divisions between two of them
        const texts = divisions.map(division => formatTime(division.index * this._settings.perDivUs));
        const widest = Math.max(0, ...texts.map(text => ctx.measureText(text).width)) + LABEL_PADDING;
        const divisionWidth = (view.right - view.left) / DIVISIONS;
        const step = LABEL_STEPS.find(candidate => candidate * divisionWidth >= widest) ?? DIVISIONS;

        // the times, centered under their tick, or ending there when the
        // right edge leaves no room
        ctx.fillStyle = canvas.getStyleVariable("--scope-text");
        for (let i = 0; i < divisions.length; i++) {
            if (divisions[i].index % step !== 0) {
                continue;
            }
            const x = Math.round(xOfTime(view, divisions[i].time));
            const halfWidth = ctx.measureText(texts[i]).width / 2;
            ctx.textAlign = x + halfWidth <= ctx.width ? "center" : "right";
            ctx.fillText(texts[i], x, (ctx.height + TICK_LENGTH) / 2);
        }
    }

    /**
     * Draws a pin in its row: its name in the color of its trace (in the
     * middle of a logic row, on the line of vref of an analog one), the scale
     * of an analog row (a line at vref and one at 0 V, ticked and labelled
     * at the left), its busy bands, then its trace, a step from each state
     * to the next, and its PWM segments (_paintPwm) where a PWM drove it.
     */
    private _paintPin(ctx: CanvasEx, pin: number, row: TraceRow, look: ScaleLook) {
        const { top, height, analog, color, start, end, left, right, xOf } = row;
        const vrefMv = this._vrefMv();
        const yOf = (states: StateList, index: number) => analog
            ? top + height - states.millivolts(index) * height / vrefMv
            : (states.level(index) !== 0 ? top : top + height);

        // the name
        ctx.font = look.fonts.normal;
        ctx.fillStyle = color;
        ctx.textAlign = "left";
        ctx.fillText(`GPIO${pin}`, 6, analog ? top : top + height / 2);

        // the scale of an analog row: its two lines, their ticks and labels
        if (analog) {
            const levels: [number, string][] = [[top, `${vrefMv / 1000} V`], [top + height, "0 V"]];
            ctx.strokeStyle = look.grid;
            ctx.lineWidth = 1;
            ctx.font = look.fonts.small;
            ctx.fillStyle = look.text;
            ctx.textAlign = "right";
            for (const [y, label] of levels) {
                const lineY = Math.round(y);
                ctx.beginPath();
                ctx.moveTo(left - TICK_LENGTH, lineY);
                ctx.lineTo(right, lineY);
                ctx.stroke();
                ctx.fillText(label, left - TICK_LENGTH - 2, lineY);
            }
        }

        const trace = this._shownTraces()[pin];
        if (trace === undefined || trace.states.length === 0) {
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

        // the PWM segments and RMT sends in the window: the states are not
        // drawn under them (the twin records none while a PWM drives the
        // pin, an RMT send leaves its state as it was), the window and each
        // of them cut out ("evenodd")
        const inWindow = (cover: { from: number; to: number }) => cover.to > start && cover.from < end;
        const covers = [...trace.pwm.filter(inWindow), ...trace.rmt.filter(inWindow)];
        ctx.save();
        if (covers.length > 0) {
            ctx.beginPath();
            ctx.rect(left, top - 2, right - left, height + 4);
            for (const cover of covers) {
                const from = xOf(Math.max(cover.from, start));
                const to = xOf(Math.min(cover.to, end));
                ctx.rect(from, top - 2, to - from, height + 4);
            }
            ctx.clip("evenodd");
        }

        // the trace: from the state at the start of the window (or the first
        // known state), a step at each change, up to the end
        const states = trace.states;
        const first = Math.max(0, firstStateAfter(states, start) - 1);
        let y = yOf(states, first);
        ctx.strokeStyle = color;
        ctx.lineWidth = 1.5;
        ctx.beginPath();
        ctx.moveTo(Math.max(left, xOf(states.time(first))), y);
        for (let i = first + 1; i < states.length && states.time(i) <= end; i++) {
            const x = xOf(states.time(i));
            ctx.lineTo(x, y);
            y = yOf(states, i);
            ctx.lineTo(x, y);
        }
        ctx.lineTo(xOf(end), y);
        ctx.stroke();
        ctx.restore();

        // the y of the state at a time (the last one at or before it)
        const stateY = (time: number) => yOf(states, Math.max(0, firstStateAfter(states, time) - 1));

        // then the PWM segments, each from the level just before it (the
        // segment it follows, else the state there) to the state after it
        // (unless another segment follows): the edges where it starts and
        // ends are drawn
        trace.pwm.forEach((segment, index) => {
            if (!inWindow(segment)) {
                return;
            }
            const previous = trace.pwm[index - 1];
            const next = trace.pwm[index + 1];
            const follows = previous !== undefined && previous.to === segment.from;
            const followed = next !== undefined && next.from === segment.to;
            const yBefore = follows ? levelY(row, pwmLevelBefore(previous, segment.from)) : stateY(segment.from);
            const yAfter = followed ? null : stateY(segment.to);
            this._paintPwm(ctx, segment, row, look, yBefore, yAfter);
        });

        // then the RMT sends, from the state before them back to the state
        // after them
        for (const train of trace.rmt) {
            if (inWindow(train)) {
                this._paintRmt(ctx, train, row, look, stateY(train.from), stateY(train.to));
            }
        }

        ctx.restore();
    }

    /**
     * Draws a band in a row from 'from' to 'to' (µs, within the window), in
     * the color of the trace seen through, 'text' written in it when it
     * fits: a signal too fast to draw (a PWM, an RMT send).
     */
    private _paintBand(ctx: CanvasEx, row: TraceRow, look: ScaleLook, from: number, to: number, text: string) {
        const { top, height, color, xOf } = row;

        // the band
        const x = xOf(from);
        const width = Math.max(1, xOf(to) - x);
        ctx.fillStyle = color;
        ctx.globalAlpha = BUSY_OPACITY;
        ctx.fillRect(x, top, width, height);
        ctx.globalAlpha = 1;

        // its text, when it fits
        ctx.font = look.fonts.small;
        if (ctx.measureText(text).width + BAND_TEXT_PADDING <= width) {
            ctx.fillStyle = look.text;
            ctx.textAlign = "center";
            ctx.fillText(text, x + width / 2, top + height / 2);
        }
    }

    /**
     * Draws a PWM segment in a row, within the window: its square signal, a
     * step at each change of level, from 'yBefore' (the line before it) when
     * it starts in the window, back to 'yAfter' when it ends there (null:
     * another segment follows, it draws that edge); or, when a period would
     * be narrower than MIN_PERIOD_PX, a band with its frequency and duty
     * cycle. High is the top of the row (vref in an analog one), low its
     * bottom.
     */
    private _paintPwm(ctx: CanvasEx, segment: PwmSegment, row: TraceRow, look: ScaleLook, yBefore: number,
                      yAfter: number | null) {
        const { color, start, end, xOf } = row;
        const from = Math.max(segment.from, start);
        const to = Math.min(segment.to, end);

        // too fast to draw: a band
        const periodPx = xOf(segment.from + 1000000 / segment.frequency) - xOf(segment.from);
        if (pwmToggles(segment) && periodPx < MIN_PERIOD_PX) {
            this._paintBand(ctx, row, look, from, to, `${frequencyText(segment.frequency)} · ${percentText(segment.duty)}`);
            return;
        }

        // the square signal: from the line before it when it starts in the
        // window, its level at the start, a step at each change
        let y = levelY(row, pwmLevelAt(segment, from));
        ctx.strokeStyle = color;
        ctx.lineWidth = 1.5;
        ctx.beginPath();
        if (segment.from >= start) {
            ctx.moveTo(xOf(from), yBefore);
            ctx.lineTo(xOf(from), y);
        } else {
            ctx.moveTo(xOf(from), y);
        }
        if (pwmToggles(segment)) {
            for (let n = pwmFirstChangeAfter(segment, from); pwmChangeTime(segment, n) < to; n++) {
                const x = xOf(pwmChangeTime(segment, n));
                ctx.lineTo(x, y);
                y = levelY(row, pwmChangeLevel(segment, n));
                ctx.lineTo(x, y);
            }
        }

        // to its end, back to the line after it when it ends in the window
        ctx.lineTo(xOf(to), y);
        if (yAfter !== null && segment.to <= end) {
            ctx.lineTo(xOf(to), yAfter);
        }
        ctx.stroke();
    }

    /**
     * Draws an RMT send in a row, within the window: its pulses, from
     * 'yBefore' (the line before it) when it starts in the window, back to
     * 'yAfter' (the line after it) when it ends there; or, when its pulses
     * would be narrower than MIN_PULSE_PX on average, a band saying what it
     * sent ("RMT · 3 bytes", "RMT · 40 pulses").
     */
    private _paintRmt(ctx: CanvasEx, train: RmtTrain, row: TraceRow, look: ScaleLook, yBefore: number,
                      yAfter: number) {
        const { color, start, end, xOf } = row;
        const from = Math.max(train.from, start);
        const to = Math.min(train.to, end);
        const count = rmtPulseCount(train);

        // too fast to draw: a band
        const averagePx = (xOf(train.to) - xOf(train.from)) / Math.max(1, count);
        if (averagePx < MIN_PULSE_PX) {
            const bytes = train.bytes.length;
            const what = bytes > 0 ? `${bytes} ${tx.twin.scope_rmt_bytes}` : `${count} ${tx.twin.scope_rmt_pulses}`;
            this._paintBand(ctx, row, look, from, to, `RMT · ${what}`);
            return;
        }

        // from the line before it when it starts in the window (else from
        // its first pulse in the window)
        let y: number | null = null;
        ctx.strokeStyle = color;
        ctx.lineWidth = 1.5;
        ctx.beginPath();
        if (train.from >= start) {
            ctx.moveTo(xOf(from), yBefore);
            y = yBefore;
        }

        // a step at each pulse in the window
        let time = train.from;
        for (let index = 0; index < count && time < to; index++) {
            const pulseEnd = time + rmtPulseTicks(train, index) * train.tickUs;
            if (pulseEnd > from) {
                const x = xOf(Math.max(time, from));
                const level = levelY(row, rmtPulseLevel(train, index));
                if (y === null) {
                    ctx.moveTo(x, level);
                } else {
                    ctx.lineTo(x, y);
                    ctx.lineTo(x, level);
                }
                y = level;
            }
            time = pulseEnd;
        }

        // to its end, back to the line after it when it ends in the window
        if (y !== null) {
            ctx.lineTo(xOf(to), y);
            if (train.to <= end) {
                ctx.lineTo(xOf(to), yAfter);
            }
        }
        ctx.stroke();
    }
}
