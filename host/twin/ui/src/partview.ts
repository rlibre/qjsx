/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The view of a part of the board, in its section of the Board panel. Three
// levels, from the least to the most work for whoever writes the part:
//   1. no view, no controls: the state the model publishes, as name / value
//      (StateList);
//   2. controls declared by the model ("static controls"): the page builds
//      them (ControlsView); a control sends "set" { name: value };
//   3. a view of its own: a module whose createView(x4, part) returns a
//      Component (a project's view.js, loaded with import(): the page gives
//      it x4js, which its bundle does not export).
import * as x4 from "x4js";
import { BoxProps, Checkbox, Component, HBox, Knob, Label, Select, Slider, VBox } from "x4js";
import { HoldButton } from "./controls/holdbutton";
import { Led } from "./controls/led";
import { PartControl, PartInfo, PartState, sendPartAction } from "./messages";

// What a view of its own gets: the part, its state, and how to act on it.
export interface PartHandle {
    id:      string;
    label:   string;
    model:   string;
    onState: (listener: (state: PartState) => void) => void;   // each state published
    send:    (action: string, args?: unknown) => void;         // twin.part.action
}

// The module of a view of its own.
interface ViewModule {
    createView: (x4js: typeof x4, part: PartHandle) => Component;
}

/**
 * Digits after the point that 'step' needs (0.1: 1, 0.25: 2, 1: 0).
 */
function decimalsOf(step: number) {
    const text = String(step);
    const point = text.indexOf(".");
    return point < 0 ? 0 : text.length - point - 1;
}

/**
 * A value of the state as text, with the unit of its control.
 */
function valueText(value: unknown, control: PartControl) {
    if (typeof value === "number") {
        const decimals = control.step !== undefined ? decimalsOf(control.step) : 2;
        return `${value.toFixed(decimals)}${control.unit ? ` ${control.unit}` : ""}`;
    }
    if (typeof value === "string") {
        return value;
    }
    return JSON.stringify(value);
}

/**
 * The colors of a "led" control: one [r, g, b], or a list of them.
 */
function colorsOf(value: unknown): number[][] {
    if (!Array.isArray(value)) {
        return [];
    }
    return Array.isArray(value[0]) ? (value as number[][]) : [value as number[]];
}

// ### 1. the state as name / value ###########################################

class StateList extends VBox {
    private _rows = new Map<string, Label>();   // field -> its value

    /**
     * Shows the fields of the state, a row each (rows added as fields come).
     */
    setState(state: PartState) {
        for (const [name, value] of Object.entries(state)) {
            if (!this._rows.has(name)) {
                const row = new Label({ cls: "value", text: "" });
                this._rows.set(name, row);
                this.appendContent(new HBox({ cls: "control", content: [new Label({ cls: "name", text: name }), row] }));
            }
            this._rows.get(name).setText(typeof value === "string" ? value : JSON.stringify(value));
        }
    }
}

// ### 2. the controls declared by the model ##################################

// A control on the page: its row, and how it shows a value of the state.
interface ControlRow {
    row:   Component;
    show:  (value: unknown) => void;
    knob?: boolean;             // a knob: stands beside the knobs next to it
}

/**
 * A slider: sends "set" { name: value } as it moves; its value shown after
 * it.
 */
function sliderControl(part: PartInfo, control: PartControl): ControlRow {
    const value = new Label({ cls: "value", text: "" });
    const slider: Slider = new Slider({
        min: control.min ?? 0,
        max: control.max ?? 100,
        step: control.step ?? 1,
        value: control.min ?? 0,
        flex: true,
        change: () => sendPartAction(part.id, "set", { [control.name]: slider.getValue() }),
    });

    const row = new HBox({ cls: "control", content: [new Label({ cls: "name", text: control.label ?? control.name }), slider, value] });
    return {
        row,
        show: shown => {
            if (typeof shown === "number") {
                slider.setValue(shown);
            }
            value.setText(valueText(shown, control));
        },
    };
}

/**
 * A knob: its name under the dial (none when the label is ""), its value
 * under the name; sends "set" { name: value } as it turns. Consecutive knobs
 * stand side by side.
 */
function knobControl(part: PartInfo, control: PartControl): ControlRow {
    const value = new Label({ cls: "value", text: "" });
    const knob: Knob = new Knob({
        min: control.min ?? 0,
        max: control.max ?? 100,
        step: control.step ?? 1,
        value: control.min ?? 0,
        size: 64,
        label: control.label === "" ? undefined : (control.label ?? control.name),
        change: () => sendPartAction(part.id, "set", { [control.name]: knob.getValue() }),
    });

    return {
        row: new VBox({ cls: "knob-control", content: [knob, value] }),
        knob: true,
        show: shown => {
            if (typeof shown === "number") {
                knob.setValue(shown);
            }
            value.setText(valueText(shown, control));
        },
    };
}

/**
 * A switch (a check box): sends "set" { name: true | false }.
 */
function switchControl(part: PartInfo, control: PartControl): ControlRow {
    const box: Checkbox = new Checkbox({
        label: control.label ?? control.name,
        change: () => sendPartAction(part.id, "set", { [control.name]: box.getCheck() }),
    });
    return { row: new HBox({ cls: "control", content: [box] }), show: shown => box.setCheck(shown === true) };
}

/**
 * A choice among 'items': sends "set" { name: item }.
 */
function selectControl(part: PartInfo, control: PartControl): ControlRow {
    const items = (control.items ?? []).map(item => ({ id: item, text: item }));
    const select: Select = new Select({
        items,
        change: () => sendPartAction(part.id, "set", { [control.name]: select.getValue() }),
    });
    const row = new HBox({ cls: "control", content: [new Label({ cls: "name", text: control.label ?? control.name }), select] });
    return { row, show: shown => select.setValue(String(shown)) };
}

/**
 * A button held down: "set" { name: true } pressed, { name: false } released.
 */
function buttonControl(part: PartInfo, control: PartControl): ControlRow {
    const button = new HoldButton({
        label: control.label ?? part.label,
        press: () => sendPartAction(part.id, "set", { [control.name]: true }),
        release: () => sendPartAction(part.id, "set", { [control.name]: false }),
    });
    return { row: new HBox({ cls: "control", content: [button] }), show: () => {} };
}

/**
 * A value shown, nothing to set.
 */
function valueControl(control: PartControl): ControlRow {
    const value = new Label({ cls: "value", text: "" });
    const row = new HBox({ cls: "control", content: [new Label({ cls: "name", text: control.label ?? control.name }), value] });
    return { row, show: shown => value.setText(valueText(shown, control)) };
}

/**
 * LEDs: one per color of the value ([r, g, b], or a list of them).
 */
function ledControl(): ControlRow {
    const line = new HBox({ cls: "control leds" });
    let leds: Led[] = [];
    return {
        row: line,
        show: shown => {
            // as many LEDs as colors (built again only when the count changes)
            const colors = colorsOf(shown);
            if (leds.length !== colors.length) {
                leds = colors.map(() => new Led({}));
                line.setContent(leds);
            }
            colors.forEach((color, index) => leds[index].setColor(color[0], color[1], color[2]));
        },
    };
}

class ControlsView extends VBox {
    private _rows = new Map<string, ControlRow>();   // control name -> its row

    /**
     * A row per control of the part.
     */
    constructor(props: BoxProps, part: PartInfo) {
        super(props);

        // the controls, consecutive knobs on one line
        const content: Component[] = [];
        let knobs: HBox = null;
        for (const control of part.controls) {
            const made = this._makeControl(part, control);
            this._rows.set(control.name, made);
            if (!made.knob) {
                knobs = null;
                content.push(made.row);
                continue;
            }
            if (knobs === null) {
                knobs = new HBox({ cls: "knobs" });
                content.push(knobs);
            }
            knobs.appendContent(made.row);
        }
        this.setContent(content);
    }

    /**
     * Shows the fields of the state its controls show.
     */
    setState(state: PartState) {
        for (const [name, control] of this._rows) {
            if (name in state) {
                control.show(state[name]);
            }
        }
    }

    /**
     * The row of a control, by its type.
     */
    private _makeControl(part: PartInfo, control: PartControl): ControlRow {
        switch (control.type) {
            case "slider": {
                return sliderControl(part, control);
            }
            case "knob": {
                return knobControl(part, control);
            }
            case "switch": {
                return switchControl(part, control);
            }
            case "select": {
                return selectControl(part, control);
            }
            case "button": {
                return buttonControl(part, control);
            }
            case "led": {
                return ledControl();
            }
            default: {
                return valueControl(control);
            }
        }
    }
}

// ### the view of a part #####################################################

export class PartView extends VBox {
    private _listeners: ((state: PartState) => void)[] = [];   // who shows the state
    private _last: PartState = null;                           // the last state, for a view that comes late

    /**
     * The view of 'part': its own (loaded), its controls, or its state.
     */
    constructor(props: BoxProps, part: PartInfo) {
        super(props);

        if (part.view !== null) {
            this._loadView(part);
        } else if (part.controls.length > 0) {
            const controls = new ControlsView({}, part);
            this._listeners.push(state => controls.setState(state));
            this.setContent(controls);
        } else {
            const list = new StateList({ cls: "state-list" });
            this._listeners.push(state => list.setState(state));
            this.setContent(list);
        }
    }

    /**
     * A state the part published.
     */
    setState(state: PartState) {
        this._last = state;
        for (const listener of this._listeners) {
            listener(state);
        }
    }

    /**
     * Loads a view of its own and gives it the part; a view that fails shows
     * why.
     */
    private async _loadView(part: PartInfo) {
        const handle: PartHandle = {
            id: part.id,
            label: part.label,
            model: part.model,
            onState: listener => {
                this._listeners.push(listener);
                if (this._last !== null) {
                    listener(this._last);
                }
            },
            send: (action, args) => sendPartAction(part.id, action, args),
        };

        try {
            const module = (await import(part.view)) as ViewModule;
            this.setContent(module.createView(x4, handle));
        } catch (error) {
            this.setContent(new Label({ cls: "error", text: `${part.view}: ${String(error)}` }));
        }
    }
}
