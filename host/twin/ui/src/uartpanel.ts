/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What the section of one UART of the board shows (device/serial in the
// twin): its TCP link (port, listening, client), the log of its traffic (what the
// program wrote, what it received from the page or from TCP), shown as text
// or in hexadecimal, and a line to send bytes to the program, as text with a
// line ending or as hexadecimal bytes.
import { BoxProps, Button, Checkbox, HBox, Label, Select, TextEdit, VBox } from "x4js";
import { sendToUart, UartRecord, UartState } from "./messages";
import { TcpControl } from "./tcpcontrol";
import { tx } from "./texts";

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
 * rx 18", then its TCP link when it listens: "· tcp 8094, client connected".
 */
export function uartSummary(state: UartState) {
    const opened = `${formatOf(state)}${state.flowControl ? " rts/cts" : ""} · ${pinsOf(state)}`;
    const client = state.tcp.client ? tx.twin.tcp_client : tx.twin.tcp_waiting;
    const tcp = state.tcp.listening ? ` · tcp ${state.tcp.port}, ${client}` : "";
    return `${state.open ? opened : tx.twin.uart_closed}${tcp}`;
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
        tcp: TcpControl;
        hexView: Checkbox;
        log: VBox;
        input: TextEdit;
        ending: Select;
        hexInput: Checkbox;
        sendBar: HBox;
    };

    private _uart: number;                  // the number of the UART
    private _records: UartRecord[] = [];    // shown in the log (the last MAX_LINES)

    /**
     * What the section of UART 'uart' shows, closed until the first state.
     */
    constructor(props: BoxProps, uart: number) {
        super({ ...props, cls: `uart-panel ${props.cls ?? ""}` });
        this._uart = uart;

        // the line endings
        const endingTexts = [tx.twin.uart_ending_none, tx.twin.uart_ending_lf, tx.twin.uart_ending_crlf];
        const endingItems = ENDINGS.map((ending, index) => ({ id: ending, text: endingTexts[index] }));

        this.setContent([
            // the TCP link
            this.refs.tcp = new TcpControl("uart", uart),

            // the display, clear
            new HBox({ cls: "uart-bar", content: [
                new Label({ cls: "uart-log-title", flex: true, text: tx.twin.uart_log }),
                this.refs.hexView = new Checkbox({
                    label: tx.twin.uart_hex_view,
                    tooltip: tx.twin.uart_hex_view_tooltip,
                    change: () => this._redraw(),
                }),
                new Button({ cls: "outline", label: tx.twin.uart_clear, click: () => this._clear() }),
            ] }),

            // the log
            this.refs.log = new VBox({ cls: "uart-log" }),

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
     * Shows the TCP link of the UART; the page can send only while the
     * program has it open.
     */
    setState(state: UartState) {
        this.refs.tcp.setState(state.tcp);
        this.refs.sendBar.enable(state.open);
    }

    /**
     * Adds records of the traffic to the log, the newest in view.
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
        this.refs.log.lastChild<Label>()?.scrollIntoView({ block: "nearest" });
    }

    /**
     * The line of a record: "→ text" for what the program wrote, "← page text"
     * or "← tcp text" for what it received.
     */
    private _line(record: UartRecord) {
        const fromProgram = record.source === "program";
        const origin = record.source === "page" ? tx.twin.uart_from_page : tx.twin.uart_from_tcp;
        const prefix = fromProgram ? "→" : `← ${origin}`;
        const bytes = this.refs.hexView.getCheck() ? asHex(record.bytes) : asText(record.bytes);
        return new Label({ cls: `uart-line ${fromProgram ? "out" : "in"}`, text: `${prefix} ${bytes}` });
    }

    /**
     * Shows the log again (text or hexadecimal changed).
     */
    private _redraw() {
        this.refs.log.setContent(this._records.map(record => this._line(record)));
        this.refs.log.lastChild<Label>()?.scrollIntoView({ block: "nearest" });
    }

    /**
     * Empties the log.
     */
    private _clear() {
        this._records = [];
        this.refs.log.clearContent();
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
