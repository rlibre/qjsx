/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "stdlib/usb" of the standard library (lib/stdlib): the board as a
// USB device, on top of the native module device/usb. It gives the composite
// device of device/usb as it is, and the common case in one object:
//
//   import { UsbSerial } from "stdlib/usb";
//   const tablet = new UsbSerial({ manufacturer: "Acme", product: "Gadget" });
//   tablet.onData(bytes => ...);
//   tablet.onLineState(({ dtr, rts }) => ...);
//   tablet.write("hello\r\n");
//   tablet.close();
//
//   import { Usb } from "stdlib/usb";          // several functions: see device/usb
import { Usb, UsbCdc } from "device/usb";

export { Usb, UsbCdc };

/**
 * A USB device with a single serial function, started at once: the
 * descriptors are those of new Usb(...) (manufacturer, product,
 * serialNumber, vendorId, productId). close() is the one of Usb.
 */
export class UsbSerial extends Usb {
    #port;

    constructor(descriptors) {
        super(descriptors);
        this.#port = this.serial();
        this.start();
    }

    /**
     * callback(bytes) for each block received (a new Uint8Array).
     */
    onData(callback) {
        this.#port.onData(callback);
    }

    /**
     * callback({ dtr, rts }) when the host opens or closes the port.
     */
    onLineState(callback) {
        this.#port.onLineState(callback);
    }

    /**
     * Sends a string (UTF-8) or bytes; what the host does not take in time
     * is dropped (nobody there, or not reading).
     */
    write(data) {
        this.#port.write(data);
    }

    /**
     * The line state now: { dtr, rts }.
     */
    lineState() {
        return this.#port.lineState();
    }
}
