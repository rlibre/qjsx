/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What the section of one UART of the board shows (device/serial in the
// twin): the log of its traffic (what the program wrote, what it received
// from the page, TCP or the PC port), shown as text or in hexadecimal, and a
// line to send bytes to the program, as text with a line ending or as
// hexadecimal bytes. Its settings are in the menu of a button the section
// shows on its title line (menuButton), marked when a link is on or failed:
// a sub menu "TCP" (the TCP port) and a sub menu "PC port" (the wiring to a
// serial port of the PC), each titled with what is on, then the log shown in
// hexadecimal (checked) and clearing the log.
import { BoxProps, Button, Checkbox, HBox, Label, Menu, MenuElement, Select, TextEdit, VBox } from "x4js";
import { LogView } from "./controls/logview";
import { sendToUart, UartRecord, UartState } from "./messages";
import { SerialControl } from "./serialcontrol";
import { TcpControl } from "./tcpcontrol";
import { tx } from "./texts";

import gearIcon from "./assets/gear.svg";

// Lines kept in the log; the oldest go first.
const MAX_LINES = 500;

// The line endings offered for the text sent (the id is what is added).
const ENDINGS = ["", "\n", "\r\n"];

/**
 * The format of a UART as it is usually written: "9600 8E1".
 */
function formatOf(state: UartState) {
    const parity = state.parity === "even" ? "E" : state.parity === "odd" ? "O" : "N";
    return `${state.baudRate} ${state.dataBits}${parity}${state.stopBits}`;
}

/**
 * The wired pins of a UART: "tx 17 rx 18".
 */
function pinsOf(state: UartState) {
    const pins = [["tx", state.tx], ["rx", state.rx], ["rts", state.rts], ["cts", state.cts]];
    return pins.filter(([, pin]) => pin !== null).map(([name, pin]) => `${name} ${pin}`).join(" ");
}

/**
 * The title line of the section of a UART: "closed", or "9600 8E1 · tx 17
 * rx 18", then its TCP link when it listens: "· tcp 8094, client connected",
 * the PC port it is wired to when connected: "· COM5", and what failed:
 * "· COM5: no such port".
 */
export function uartSummary(state: UartState) {
    const opened = `${formatOf(state)}${state.flowControl ? " rts/cts" : ""} · ${pinsOf(state)}`;
    const client = state.tcp.client ? tx.twin.tcp_client : tx.twin.tcp_waiting;
    const tcp = state.tcp.listening ? ` · tcp ${state.tcp.port}, ${client}` : "";
    const tcpError = state.tcp.error !== "" ? ` · tcp: ${state.tcp.error}` : "";
    const serial = state.serial.connected ? ` · ${state.serial.port}` : "";
    const serialError = state.serial.error !== "" ? ` · ${state.serial.port}: ${state.serial.error}` : "";
    return `${state.open ? opened : tx.twin.uart_closed}${tcp}${tcpError}${serial}${serialError}`;
}

/**
 * The word of the source of what the program received, in the log (read
 * when needed: the texts are declared after the modules load).
 */
function originOf(source: string) {
    const origins: Record<string, string> = {
        page:   tx.twin.uart_from_page,
        tcp:    tx.twin.uart_from_tcp,
        serial: tx.twin.uart_from_serial,
    };
    return origins[source] ?? source;
}

/**
 * Bytes as text: UTF-8, the control characters written as escapes (\r, \n,
 * \t, \xNN) so that they can be seen.
 */
function asText(bytes: number[]) {
    const text = new TextDecoder().decode(new Uint8Array(bytes));
    const named: Record<string, string> = { "\r": "\\r", "\n": "\\n", "\t": "\\t" };
    return text.replace(/[\x00-\x1f\x7f]/g, char => {
        return named[char] ?? `\\x${char.charCodeAt(0).toString(16).padStart(2, "0")}`;
    });
}

/**
 * Bytes in hexadecimal: "61 62 0D".
 */
function asHex(bytes: number[]) {
    return bytes.map(byte => byte.toString(16).toUpperCase().padStart(2, "0")).join(" ");
}

/**
 * Hexadecimal bytes typed by the user ("02 A0 ff", "02a0ff"): the bytes, or
 * null if it is not an even count of hexadecimal digits.
 */
function parseHex(text: string): Uint8Array | null {
    const digits = text.replace(/\s+/g, "");
    if (digits.length % 2 !== 0 || !/^[0-9a-fA-F]*$/.test(digits)) {
        return null;
    }

    const bytes = new Uint8Array(digits.length / 2);
    for (let i = 0; i < bytes.length; i++) {
        bytes[i] = parseInt(digits.slice(i * 2, i * 2 + 2), 16);
    }
    return bytes;
}

export class UartPanel extends VBox {
    declare protected refs: {
        log: LogView;
        input: TextEdit;
        ending: Select;
        hexInput: Checkbox;
        sendBar: HBox;
    };

    private _uart: number;                  // the number of the UART
    private _records: UartRecord[] = [];    // shown in the log (the last MAX_LINES)
    private _hexView = false;               // the log shows the bytes in hexadecimal
    private _state: UartState = null;       // the last state, for the menu of the links
    private _ports: string[] = [];          // the PC's serial ports, as last given
    private _links: { tcp: TcpControl; serial: SerialControl } = null;   // the controls of the last menu opened

    // The settings button, for the title line of the section: its menu holds
    // the TCP port, the PC port, how the traffic is shown.
    readonly menuButton: Button;

    /**
     * What the section of UART 'uart' shows, closed until the first state.
     */
    constructor(props: BoxProps, uart: number) {
        super({ ...props, cls: `uart-panel ${props.cls ?? ""}` });
        this._uart = uart;

        // the line endings
        const endingTexts = [tx.twin.uart_ending_none, tx.twin.uart_ending_lf, tx.twin.uart_ending_crlf];
        const endingItems = ENDINGS.map((ending, index) => ({ id: ending, text: endingTexts[index] }));

        // the settings button, shown by the section
        this.menuButton = new Button({
            cls: "outline menu-button",
            icon: gearIcon,
            tooltip: tx.twin.uart_links_tooltip,
            menu: () => this._linksMenu(),
        });

        this.setContent([
            // the log
            this.refs.log = new LogView({ cls: "uart-log" }),

            // what the page sends
            this.refs.sendBar = new HBox({ cls: "uart-bar", disabled: true, content: [
                this.refs.input = new TextEdit({ flex: true, label: "", type: "text", placeholder: tx.twin.uart_text_placeholder }),
                this.refs.ending = new Select({ items: endingItems, value: "\n" }),
                this.refs.hexInput = new Checkbox({
                    label: tx.twin.uart_hex_input,
                    tooltip: tx.twin.uart_hex_input_tooltip,
                    change: () => this._onHexInput(),
                }),
                new Button({ label: tx.twin.uart_send, click: () => this._send() }),
            ] }),
        ]);

        // Enter in the input sends
        this.refs.input.getInput().addShortcut("Enter", () => this._send(), { editable: true });
    }

    /**
     * Takes the state of the UART ('ports': the PC's serial ports): the
     * button of the links marked (a link on, a link failed), the menu updated
     * if it is open; the page can send only while the program has it open.
     */
    setState(state: UartState, ports: string[]) {
        this._state = state;
        this._ports = ports;

        // the button: a link on, a link failed
        const failed = state.tcp.error !== "" || state.serial.error !== "";
        const on = state.tcp.listening || state.serial.connected;
        this.menuButton.setClass("error", failed);
        this.menuButton.setClass("on", on && !failed);

        // the menu, while it is open
        this._showLinks();
        this.refs.sendBar.enable(state.open);
    }

    /**
     * The menu of the section, built at each opening (the controls of the
     * links kept up to date while open): a sub menu per link, its title
     * saying what is on ("TCP · 8094", "PC port · COM5"), then the log in
     * hexadecimal (checked when it is) and clearing it.
     */
    private _linksMenu(): MenuElement[] {
        // the controls, with the last state
        const tcp = new TcpControl("uart", this._uart);
        const serial = new SerialControl(this._uart);
        this._links = { tcp, serial };
        this._showLinks();

        // what is on, for the titles
        const tcpOn = this._state?.tcp.listening ? ` · ${this._state.tcp.port}` : "";
        const serialOn = this._state?.serial.connected ? ` · ${this._state.serial.port}` : "";

        // a sub menu each, then the log
        return [
            { text: `${tx.twin.uart_link_tcp}${tcpOn}`, menu: new Menu({ items: [new HBox({ cls: "link-menu", content: tcp })] }) },
            { text: `${tx.twin.serial_port}${serialOn}`, menu: new Menu({ items: [new HBox({ cls: "link-menu", content: serial })] }) },
            "-",
            { text: tx.twin.uart_hex_view, checked: this._hexView, click: () => this._setHexView(!this._hexView) },
            { text: tx.twin.uart_clear, click: () => this._clear() },
        ];
    }

    /**
     * The log in hexadecimal or as text.
     */
    private _setHexView(hex: boolean) {
        this._hexView = hex;
        this._redraw();
    }

    /**
     * Shows the last state in the controls of the menu (none before the first
     * opening; those of a closed menu are no longer seen).
     */
    private _showLinks() {
        if (this._links === null || this._state === null) {
            return;
        }
        this._links.tcp.setState(this._state.tcp);
        this._links.serial.setState(this._state.serial, this._ports);
    }

    /**
     * Adds records of the traffic to the log, the newest in view unless the
     * user scrolled up (LogView).
     */
    addRecords(records: UartRecord[]) {
        // the lines, the oldest dropped past MAX_LINES
        for (const record of records) {
            this._records.push(record);
            this.refs.log.appendContent(this._line(record));
        }
        while (this._records.length > MAX_LINES) {
            this._records.shift();
            this.refs.log.removeChild(this.refs.log.firstChild());
        }

        // the newest in view
        this.refs.log.followNewest();
    }

    /**
     * The line of a record: "→ text" for what the program wrote, "← page
     * text", "← tcp text" or "← serial text" for what it received.
     */
    private _line(record: UartRecord) {
        const fromProgram = record.source === "program";
        const origin = originOf(record.source);
        const prefix = fromProgram ? "→" : `← ${origin}`;
        const bytes = this._hexView ? asHex(record.bytes) : asText(record.bytes);
        return new Label({ cls: `uart-line ${fromProgram ? "out" : "in"}`, text: `${prefix} ${bytes}` });
    }

    /**
     * Shows the log again (text or hexadecimal changed).
     */
    private _redraw() {
        this.refs.log.setContent(this._records.map(record => this._line(record)));
        this.refs.log.followNewest();
    }

    /**
     * Empties the log.
     */
    private _clear() {
        this._records = [];
        this.refs.log.clearLines();
    }

    /**
     * Hexadecimal input on or off: the placeholder says what to type, a line
     * ending makes no sense for bytes.
     */
    private _onHexInput() {
        const hex = this.refs.hexInput.getCheck();
        this.refs.input.getInput().setAttribute("placeholder", hex ? tx.twin.uart_hex_placeholder : tx.twin.uart_text_placeholder);
        this.refs.ending.enable(!hex);
        this.refs.input.removeClass("invalid");
    }

    /**
     * Sends what was typed to the program: hexadecimal bytes, or the text
     * (UTF-8) with its line ending; the input is emptied. Hexadecimal that
     * does not read stays, marked.
     */
    private _send() {
        const typed = this.refs.input.getValue();

        // the bytes
        let bytes: Uint8Array;
        if (this.refs.hexInput.getCheck()) {
            bytes = parseHex(typed);
        } else {
            bytes = new TextEncoder().encode(typed + String(this.refs.ending.getValue()));
        }

        // hexadecimal that does not read
        if (bytes === null) {
            this.refs.input.addClass("invalid");
            this.refs.input.setAttribute("title", tx.twin.uart_bad_hex);
            return;
        }

        // sent
        this.refs.input.removeClass("invalid");
        this.refs.input.setAttribute("title", null);
        if (bytes.length > 0) {
            sendToUart(this._uart, bytes);
        }
        this.refs.input.setValue("");
    }
}
