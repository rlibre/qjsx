/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What the section of the USB device shows (boards whose firmware has
// device/usb): for each serial function a USB device may have, whether the
// program uses it, its host (a TCP client plays it: DTR and RTS at 1) and
// its TCP link, which listens only while the device is started. The title
// line of the section says whether the program started the device, and its
// descriptors (usbSummary).
import { HBox, Icon, Label, VBox } from "x4js";
import { UsbState } from "./messages";
import { TcpControl } from "./tcpcontrol";
import { tx } from "./texts";

import plugIcon from "./assets/plug.svg";

// Serial functions a device may have (hal::usb::MAX_SERIALS).
const MAX_SERIALS = 2;

/**
 * A USB id as written in a descriptor: "0x303A".
 */
function usbId(id: number) {
    return `0x${id.toString(16).toUpperCase().padStart(4, "0")}`;
}

/**
 * The title line of the section: "not started by the program", or "started:
 * Acme / Gadget (0x303A:0x4001, v2.0.0)".
 */
export function usbSummary(usb: UsbState) {
    if (!usb.started) {
        return tx.twin.usb_stopped;
    }

    const serial = usb.serialNumber !== "" ? `, ${usb.serialNumber}` : "";
    const ids = `${usbId(usb.vendorId)}:${usbId(usb.productId)}`;
    return `${tx.twin.usb_started}: ${usb.manufacturer} / ${usb.product} (${ids}${serial})`;
}

class UsbFunctionRow extends HBox {
    declare protected refs: { icon: Icon; state: Label; tcp: TcpControl };

    private _index: number;   // the number of the function

    /**
     * The row of serial function 'index'.
     */
    constructor(index: number) {
        super({ cls: "usb-function unused" });
        this._index = index;

        this.setContent([
            this.refs.icon = new Icon({ cls: "usb-function-icon", iconId: plugIcon }),
            new Label({ cls: "usb-function-name", text: `${tx.twin.usb_serial} ${index}` }),
            this.refs.state = new Label({ cls: "usb-function-state", text: tx.twin.usb_device_stopped }),
            this.refs.tcp = new TcpControl("usb", index),
        ]);
    }

    /**
     * Shows the function:
     * - unused: the device is not started, or the program declared fewer
     *   serial functions;
     * - used: its host connected, or awaited.
     */
    update(usb: UsbState) {
        // used by the program or not, and why not
        const used = usb.started && this._index < usb.serials;
        const tcp = usb.functions[this._index].tcp;
        let state = tx.twin.usb_device_stopped;
        if (usb.started && !used) {
            state = tx.twin.usb_not_declared;
        } else if (used) {
            state = tcp.client ? tx.twin.usb_host : tx.twin.usb_no_host;
        }

        // the icon greyed when unused, marked when a host is there
        this.setClass("unused", !used);
        this.setClass("connected", used && tcp.client);
        this.refs.state.setText(state);
        this.refs.tcp.setState(tcp);
    }
}

export class UsbView extends VBox {
    private _rows: UsbFunctionRow[] = [];   // one per possible serial function

    /**
     * The functions, unused until the first state.
     */
    constructor() {
        super({ cls: "usb-view" });

        for (let index = 0; index < MAX_SERIALS; index++) {
            this._rows.push(new UsbFunctionRow(index));
        }
        this.setContent(this._rows);
    }

    /**
     * Shows the functions of the device.
     */
    setUsb(usb: UsbState) {
        this._rows.forEach(row => row.update(usb));
    }
}
