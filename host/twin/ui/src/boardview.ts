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
// The sections can be moved: a section dragged by its title line drops
// before or after another (a line shows where); the order is kept by the
// browser by the keys of the sections, a section it does not know at the
// end.
import { BoxProps, Component, dragManager, DropInfo, HBox, Label, VBox } from "x4js";
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

// Name under which the browser keeps the order of the sections.
const ORDER_KEY = "qjsx.twin.sections";

// Where a dragged section would drop: before or after a section.
interface DropPlace {
    section: Section;
    after:   boolean;
}

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
    declare protected refs: { name: Label; description: Label; sections: VBox };

    private _order: { keys: string[] };                         // the keys of the sections, as the user put them (kept by the browser)
    private _partList: Section[] = [];                           // the sections of the parts (setParts)
    private _deviceList: Section[] = [];                         // the sections of the devices (setBoard)
    private _dropPlace: DropPlace = null;                        // where the section dragged would drop

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
        this._order = this.loadPState(ORDER_KEY, { keys: [] });

        this.setContent([
            this.refs.name = new Label({ cls: "board-name", text: "" }),
            this.refs.description = new Label({ cls: "board-description", text: "" }),
            this.refs.sections = new VBox({ cls: "sections" }),
        ]);

        // a section dragged by its title line drops among the others
        dragManager.registerDropTarget(
            this.refs.sections,
            (command, dragged, infos) => this._onDrop(command, dragged, infos),
            dragged => this._sectionOf(dragged) !== null,
        );
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
            const section = new Section({
                title: `${tx.twin.uart} ${uart}`,
                persist: `uart${uart}`,
                content: view,
                tools: [view.menuButton],
            });
            this._uarts.set(uart, { section, view });
            sections.push(section);
        }

        // the I2C buses, hidden until the program opens one with no part on it
        this._i2c.clear();
        const busCount = board.natives.includes("device/i2c") ? I2C_BUSES : 0;
        for (let bus = 0; bus < busCount; bus++) {
            const view = new I2cPanel({});
            const section = new Section({
                title: `${tx.twin.i2c} ${bus}`,
                persist: `i2c${bus}`,
                content: view,
                tools: [view.menuButton],
            });
            section.hide();
            this._i2c.set(bus, { section, view });
            sections.push(section);
        }

        this._deviceList = sections;
        this._arrange();
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

        this._partList = sections;
        this._arrange();
        this.setPins(this._pins);
    }

    // ### Moving the sections ################################################

    /**
     * Shows every section in the order kept: the known keys first, in that
     * order, then the others as they come (the parts, then the devices).
     */
    private _arrange() {
        const all = [...this._partList, ...this._deviceList];
        const rank = (section: Section) => {
            const index = this._order.keys.indexOf(section.key);
            return index < 0 ? this._order.keys.length : index;
        };
        const sorted = all.map((section, index) => ({ section, index }))
            .sort((a, b) => rank(a.section) - rank(b.section) || a.index - b.index)
            .map(entry => entry.section);
        this.refs.sections.setContent(sorted);
    }

    /**
     * The section whose title line is 'dragged', or null (not a section of
     * this board).
     */
    private _sectionOf(dragged: Component): Section {
        const section = dragged?.parentElement(Section) ?? null;
        const ours = section !== null && [...this._partList, ...this._deviceList].includes(section);
        return ours ? section : null;
    }

    /**
     * A section dragged over the list: the line where it would drop follows
     * the pointer (before or after the section under it, by its half); on
     * the drop, the section moves there and the order is kept.
     */
    private _onDrop(command: "enter" | "leave" | "drag" | "drop", dragged: Component, infos: DropInfo) {
        if (command === "leave") {
            this._showDropPlace(null);
            return;
        }
        if (command !== "drag" && command !== "drop") {
            return;
        }

        // the place under the pointer
        const moving = this._sectionOf(dragged);
        const place = this._placeAt(infos.pt.y, moving);
        if (command === "drag") {
            this._showDropPlace(place);
            return;
        }

        // the drop
        this._showDropPlace(null);
        if (moving !== null && place !== null) {
            this._move(moving, place);
        }
    }

    /**
     * Where a section dropped at 'y' (page coordinates) would go: before or
     * after the shown section under it; null over the section itself.
     */
    private _placeAt(y: number, moving: Section): DropPlace {
        const shown = this.refs.sections.enumChildComponents(false).filter(child => child.isVisible()) as Section[];
        for (const section of shown) {
            const rect = section.getBoundingRect();
            const top = rect.top + window.scrollY;
            if (y < top + rect.height) {
                const after = y > top + rect.height / 2;
                return section === moving ? null : { section, after };
            }
        }
        const last = shown[shown.length - 1];
        return last === undefined || last === moving ? null : { section: last, after: true };
    }

    /**
     * Marks the section where the drop would go (a line above or under it),
     * the previous mark taken off.
     */
    private _showDropPlace(place: DropPlace) {
        if (this._dropPlace !== null) {
            this._dropPlace.section.removeClass("drop-before drop-after");
        }
        this._dropPlace = place;
        if (place !== null) {
            place.section.addClass(place.after ? "drop-after" : "drop-before");
        }
    }

    /**
     * Moves a section before or after another, keeps the order of every
     * section shown or not, and shows them again in it.
     */
    private _move(moving: Section, place: DropPlace) {
        const order = (this.refs.sections.enumChildComponents(false) as Section[]).filter(section => section !== moving);
        const index = order.indexOf(place.section) + (place.after ? 1 : 0);
        order.splice(index, 0, moving);
        this._order.keys = order.map(section => section.key);
        this._arrange();
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
     * Shows the UARTs ('ports': the PC's serial ports they can be wired to).
     */
    setUarts(uarts: UartState[], ports: string[]) {
        for (const state of uarts) {
            const shown = this._uarts.get(state.number);
            shown?.section.setSummary(uartSummary(state));
            shown?.view.setState(state, ports);
        }
    }

    /**
     * Adds records of the traffic of UART 'uart' to its log.
     */
    addUartRecords(uart: number, records: UartRecord[]) {
        this._uarts.get(uart)?.view.addRecords(records);
    }
}
