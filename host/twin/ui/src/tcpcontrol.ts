/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The TCP link of a UART or of a USB serial function: its port, a toggle
// button to listen, and what is going on (not listening, no client, client
// connected, or why listening failed). The twin applies and keeps the
// choices (messages.ts: setTcpLink); this control shows what the twin says.
import { Checkbox, EvFocus, HBox, Label, TextEdit } from "x4js";
import { setTcpLink, TcpKind, TcpState } from "./messages";
import { warn } from "./notify";
import { tx } from "./texts";

export class TcpControl extends HBox {
    declare protected refs: { port: TextEdit; listen: Checkbox; status: Label };

    private _kind: TcpKind;            // what the link is for
    private _index: number;            // the number of the UART or of the USB function
    private _shownPort = 0;            // the port the twin has, as shown last

    /**
     * The control of the link of UART or USB function 'index'.
     */
    constructor(kind: TcpKind, index: number) {
        super({ cls: "tcp-control" });
        this._kind = kind;
        this._index = index;

        // the port (applied when the field loses the focus), listen, then
        // what is going on
        this.setContent([
            this.refs.port = new TextEdit({
                cls: "link-field",
                label: tx.twin.tcp_port,
                type: "number",
                min: 1,
                max: 65535,
                focus: (event: EvFocus) => {
                    if (event.focus_out) {
                        this._commitPort();
                    }
                },
            }),
            this.refs.listen = new Checkbox({
                cls: "button link-action",
                label: tx.twin.tcp_listen,
                tooltip: tx.twin.tcp_listen_tooltip,
                change: () => this._request(this.refs.listen.getCheck()),
            }),
            this.refs.status = new Label({ cls: "link-status", text: tx.twin.tcp_off }),
        ]);

        // Enter in the port applies it
        this.refs.port.getInput().addShortcut("Enter", () => this._commitPort(), { editable: true });
    }

    /**
     * Shows what the twin says of the link: its port (unless the user is
     * typing one), the listening asked, and what is going on.
     */
    setState(tcp: TcpState) {
        // the port, not while it is being typed
        const isTyping = this.refs.port.getInput().hasFocus();
        const portChanged = tcp.port !== this._shownPort;
        if (!isTyping && portChanged) {
            this.refs.port.setValue(tcp.port > 0 ? String(tcp.port) : "");
        }

        // the port and the listening the twin has
        this._shownPort = tcp.port;
        this.refs.listen.setCheck(tcp.listen);

        // what is going on
        let status = tx.twin.tcp_off;
        if (tcp.error !== "") {
            status = tcp.error;
        } else if (tcp.listening) {
            status = tcp.client ? tx.twin.tcp_client : tx.twin.tcp_waiting;
        } else if (tcp.listen) {
            status = tx.twin.tcp_when_started;
        }

        this.refs.status.setText(status);
        this.refs.status.setClass("error", tcp.error !== "");
        this.refs.status.setClass("client", tcp.client);
    }

    /**
     * The port typed, applied if it changed (with the listening as it is).
     */
    private _commitPort() {
        const portChanged = this._typedPort() !== this._shownPort;
        if (portChanged) {
            this._request(this.refs.listen.getCheck());
        }
    }

    /**
     * The port typed: 1 to 65535, else 0.
     */
    private _typedPort() {
        const port = this.refs.port.getInput().getNumValue(0);
        const valid = Number.isInteger(port) && port >= 1 && port <= 65535;
        return valid ? port : 0;
    }

    /**
     * Asks the twin for the port typed and listening on or off; listening
     * without a port is refused here, with a warning.
     */
    private _request(listen: boolean) {
        // listen without a port: unchecked, the user warned
        const port = this._typedPort();
        const isRefused = listen && port === 0;
        if (isRefused) {
            this.refs.listen.setCheck(false);
            warn(tx.twin.tcp_port, tx.twin.tcp_no_port);
            return;
        }

        // asked to the twin
        setTcpLink(this._kind, this._index, port, listen);
    }
}
