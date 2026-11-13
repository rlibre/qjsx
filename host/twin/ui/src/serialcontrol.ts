/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The wiring of a UART to a real serial port of the PC: the port (the PC's
// ports, as the twin lists them), a toggle button to connect, the speed
// (real, or immediate), and what is going on (not connected, waiting for the
// program to open the UART, connected, or why the port could not open). The
// twin applies and keeps the choices (messages.ts: wireUart); this control
// shows what the twin says.
import { Checkbox, Combobox, HBox, Label, ListItem } from "x4js";
import { SerialState, wireUart } from "./messages";
import { warn } from "./notify";
import { tx } from "./texts";

// The id of the first item of the list of the ports (no port).
const NO_PORT = "";

export class SerialControl extends HBox {
    declare protected refs: { port: Combobox; connect: Checkbox; realSpeed: Checkbox; status: Label };

    private _uart: number;             // the number of the UART
    private _listed = "\u0000";        // the ports in the list, as a key (none yet)
    private _chosen = NO_PORT;         // the port shown as chosen

    /**
     * The control of the wiring of UART 'uart'.
     */
    constructor(uart: number) {
        super({ cls: "serial-control" });
        this._uart = uart;

        this.setContent([
            this.refs.port = new Combobox({
                cls: "link-field",
                label: tx.twin.serial_port,
                items: [],
                selectionChange: () => this._request(),
            }),
            this.refs.connect = new Checkbox({
                cls: "button link-action",
                label: tx.twin.serial_connect,
                tooltip: tx.twin.serial_connect_tooltip,
                change: () => this._request(),
            }),
            this.refs.realSpeed = new Checkbox({
                cls: "link-option",
                label: tx.twin.serial_real_speed,
                tooltip: tx.twin.serial_real_speed_tooltip,
                change: () => this._request(),
            }),
            this.refs.status = new Label({ cls: "link-status", text: tx.twin.serial_off }),
        ]);
    }

    /**
     * Shows what the twin says of the wiring: the PC's ports (the one chosen
     * kept in the list even when it is gone; none at all: said in the field,
     * nothing to connect), the port chosen, the wiring and the speed asked,
     * and what is going on.
     */
    setState(serial: SerialState, ports: string[]) {
        // the list, filled again only when the ports changed
        const names = ports.includes(serial.port) || serial.port === "" ? ports : [serial.port, ...ports];
        const key = names.join("\n");
        const listChanged = key !== this._listed;
        if (listChanged) {
            this._listed = key;
            const items: ListItem[] = names.map(name => ({ id: name, text: name }));
            this.refs.port.setItems(items);
        }

        // the port chosen, or what the field says without one
        if (listChanged || serial.port !== this._chosen) {
            this._chosen = serial.port;
            const none = names.length === 0 ? tx.twin.serial_none : tx.twin.serial_choose;
            if (serial.port !== NO_PORT) {
                this.refs.port.selectItem(serial.port);
            } else {
                this.refs.port.selectItem(null);
                this.refs.port.setValue(none);
            }
        }

        // nothing to choose: the field and connect off
        const nothing = names.length === 0;
        this.refs.port.enable(!nothing);
        this.refs.connect.enable(!nothing);

        // what is asked
        this.refs.connect.setCheck(serial.connect);
        this.refs.realSpeed.setCheck(serial.realSpeed);

        // what is going on
        let status = tx.twin.serial_off;
        if (serial.error !== "") {
            status = serial.error;
        } else if (serial.connected) {
            status = tx.twin.serial_connected;
        } else if (serial.connect) {
            status = tx.twin.serial_waiting;
        }
        this.refs.status.setText(serial.fixed ? `${status} (${tx.twin.serial_fixed})` : status);
        this.refs.status.setClass("error", serial.error !== "");
        this.refs.status.setClass("connected", serial.connected);
    }

    /**
     * Asks the twin for the port chosen, the wiring and the speed; wiring
     * without a port is refused here, with a warning.
     */
    private _request() {
        const port = String(this.refs.port.getSelection() ?? NO_PORT);
        const connect = this.refs.connect.getCheck();
        if (connect && port === NO_PORT) {
            this.refs.connect.setCheck(false);
            warn(tx.twin.serial_port, tx.twin.serial_no_port);
            return;
        }
        wireUart(this._uart, port, connect, this.refs.realSpeed.getCheck());
    }
}
