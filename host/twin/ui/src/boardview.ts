/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The board: its name, then one folding section (controls/section.ts) per
// device of the board, each with a short summary on its title line:
//   - each part on the board, the board's and the project's (partview.ts:
//     its controls, its state, or a view of its own);
//   - the USB device, when the firmware has device/usb (usbview.ts);
//   - each UART but the agent's link, when the firmware has device/serial
//     (uartpanel.ts);
//   - each I2C bus the program opened with no part on it, when the firmware
//     has device/i2c (i2cpanel.ts: the trace of the transfers).
import { BoxProps, HBox, Label, VBox } from "x4js";
import { Section } from "./controls/section";
import { I2cPanel, i2cSummary } from "./i2cpanel";
import { BoardInfo, I2cBusState, I2cRecord, PartInfo, PartState, PinState, UartRecord, UartState, UsbState } from "./messages";
import { pinMode } from "./pintable";
import { PartView } from "./partview";
import { tx } from "./texts";
import { UartPanel, uartSummary } from "./uartpanel";
import { usbSummary, UsbView } from "./usbview";

// The model of the push buttons: they share one section.
const BUTTON_MODEL = "button";

// I2C buses of a board with device/i2c (as twin_i2c.cpp).
const I2C_BUSES = 2;

// A section and what it shows.
interface Shown<T> {
    section: Section;
    view:    T;
}

/**
 * The title of a part: its label, then what it is (its model; the folder
 * name for a part of the project).
 */
function partTitle(part: PartInfo) {
    const model = part.model.split("/").filter(name => name !== "" && name !== ".").pop() ?? part.model;
    return `${part.label} · ${model}`;
}

/**
 * The summary of a part on its title line: its pins, and what the program
 * uses each one for when it does ("GPIO6 adc 1.65 V", "GPIO8 i2c"), from
 * the state of the pins (none yet: the numbers only).
 */
function partSummary(part: PartInfo, pins: PinState[]) {
    return part.pins.map(pin => {
        const mode = pins[pin] !== undefined ? pinMode(pins[pin]) : "-";
        return mode === "-" ? `GPIO${pin}` : `GPIO${pin} ${mode}`;
    }).join(", ");
}

export class BoardView extends VBox {
    declare protected refs: { name: Label; description: Label; parts: VBox; sections: VBox };

    private _parts = new Map<string, PartView>();                // by id of the part
    private _partSections: { part: PartInfo; section: Section }[] = [];   // the sections of the parts (not the buttons)
    private _buttonSection: { parts: PartInfo[]; section: Section } = null;   // the section "Buttons"; null: no button
    private _pins: PinState[] = [];                              // the last state of the pins
    private _usb: Shown<UsbView> = null;                         // null: the firmware has no device/usb
    private _uarts = new Map<number, Shown<UartPanel>>();        // by UART number
    private _i2c = new Map<number, Shown<I2cPanel>>();           // by bus number

    /**
     * An empty board, until setBoard().
     */
    constructor(props: BoxProps) {
        super(props);

        this.setContent([
            this.refs.name = new Label({ cls: "board-name", text: "" }),
            this.refs.description = new Label({ cls: "board-description", text: "" }),
            this.refs.parts = new VBox({ cls: "sections" }),
            this.refs.sections = new VBox({ cls: "sections" }),
        ]);
    }

    /**
     * Shows a board: its name, then a section for the USB device and per
     * UART (the parts come with setParts).
     */
    setBoard(board: BoardInfo) {
        // the name
        this.refs.name.setText(board.name);
        this.refs.description.setText(board.description ?? "");
        document.title = `${board.name} - ${tx.twin.title}`;

        // the USB device
        const sections: Section[] = [];
        this._usb = null;
        if (board.natives.includes("device/usb")) {
            const view = new UsbView();
            this._usb = { section: new Section({ title: tx.twin.usb_title, persist: "usb", content: view }), view };
            sections.push(this._usb.section);
        }

        // the UARTs, but the agent's link
        this._uarts.clear();
        const uartCount = board.natives.includes("device/serial") ? board.uarts ?? 0 : 0;
        for (let uart = 0; uart < uartCount; uart++) {
            if (uart === board.linkUart) {
                continue;
            }
            const view = new UartPanel({}, uart);
            const section = new Section({ title: `${tx.twin.uart} ${uart}`, persist: `uart${uart}`, content: view });
            this._uarts.set(uart, { section, view });
            sections.push(section);
        }

        // the I2C buses, hidden until the program opens one with no part on it
        this._i2c.clear();
        const busCount = board.natives.includes("device/i2c") ? I2C_BUSES : 0;
        for (let bus = 0; bus < busCount; bus++) {
            const view = new I2cPanel({});
            const section = new Section({ title: `${tx.twin.i2c} ${bus}`, persist: `i2c${bus}`, content: view });
            section.hide();
            this._i2c.set(bus, { section, view });
            sections.push(section);
        }

        this.refs.sections.setContent(sections);
    }

    /**
     * Shows the I2C buses: the section of a bus only while the program has it
     * open and no part is wired on it (a part shows itself).
     */
    setI2c(buses: I2cBusState[]) {
        for (const state of buses) {
            const shown = this._i2c.get(state.bus);
            shown?.section.show(state.open && state.parts.length === 0);
            shown?.section.setSummary(i2cSummary(state));
        }
    }

    /**
     * Adds transfers to the trace of their bus.
     */
    addI2cRecords(records: I2cRecord[]) {
        for (const [bus, shown] of this._i2c) {
            const own = records.filter(record => record.bus === bus);
            if (own.length > 0) {
                shown.view.addRecords(own);
            }
        }
    }

    /**
     * Shows the parts on the board: the push buttons together in one
     * section "Buttons", side by side; every other part in a section of its
     * own, in the order of the wiring (the board's first).
     */
    setParts(parts: PartInfo[]) {
        this._parts.clear();

        // the view of each part
        for (const part of parts) {
            this._parts.set(part.id, new PartView({ cls: "part" }, part));
        }

        // the buttons: one section, first
        this._buttonSection = null;
        this._partSections = [];
        const buttons = parts.filter(part => part.model === BUTTON_MODEL);
        const sections: Section[] = [];
        if (buttons.length > 0) {
            const line = new HBox({ cls: "buttons", content: buttons.map(part => this._parts.get(part.id)) });
            const section = new Section({ title: tx.twin.buttons, persist: "buttons", content: line });
            this._buttonSection = { parts: buttons, section };
            sections.push(section);
        }

        // the other parts: a section each
        for (const part of parts.filter(item => item.model !== BUTTON_MODEL)) {
            const section = new Section({ title: partTitle(part), persist: `part-${part.id}`, content: this._parts.get(part.id) });
            this._partSections.push({ part, section });
            sections.push(section);
        }

        this.refs.parts.setContent(sections);
        this.setPins(this._pins);
    }

    /**
     * Follows the state of the pins in the summaries of the parts.
     */
    setPins(pins: PinState[]) {
        this._pins = pins;
        for (const { part, section } of this._partSections) {
            section.setSummary(partSummary(part, pins));
        }
        if (this._buttonSection !== null) {
            const summaries = this._buttonSection.parts.map(part => `${part.label}: ${partSummary(part, pins)}`);
            this._buttonSection.section.setSummary(summaries.join(", "));
        }
    }

    /**
     * Shows what a part published.
     */
    setPartState(id: string, state: PartState) {
        this._parts.get(id)?.setState(state);
    }

    /**
     * Shows the USB device.
     */
    setUsb(usb: UsbState) {
        this._usb?.section.setSummary(usbSummary(usb));
        this._usb?.view.setUsb(usb);
    }

    /**
     * Shows the UARTs.
     */
    setUarts(uarts: UartState[]) {
        for (const state of uarts) {
            const shown = this._uarts.get(state.number);
            shown?.section.setSummary(uartSummary(state));
            shown?.view.setState(state);
        }
    }

    /**
     * Adds records of the traffic of UART 'uart' to its log.
     */
    addUartRecords(uart: number, records: UartRecord[]) {
        this._uarts.get(uart)?.view.addRecords(records);
    }
}
