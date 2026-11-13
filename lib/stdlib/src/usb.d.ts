/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * The board as a USB device, from the standard library (written in JS on top
 * of the native module `device/usb`): it re-exports {@link Usb} and
 * {@link UsbCdc} as they are, and adds {@link UsbSerial}, the common case of
 * a device with one serial function, in one object. A program thus needs a
 * single import.
 *
 * @example
 * import { UsbSerial } from "stdlib/usb";
 *
 * const tablet = new UsbSerial({ manufacturer: "Acme", product: "Gadget" });
 * tablet.onData(bytes => tablet.write(bytes));          // echo
 * tablet.onLineState(({ dtr }) => console.log(dtr ? "opened" : "closed"));
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | — | ✓ ¹ | — | ✓ |
 *
 * ¹ Where `device/usb` is: the chips with USB OTG (ESP32-S2, S3, P4); importing it fails elsewhere.
 *
 * @module stdlib/usb
 */
declare module "stdlib/usb" {
    import { Usb } from "device/usb";
    import type { UsbDescriptors, UsbData, UsbLineState } from "device/usb";

    /**
     * The classes of `device/usb`, re-exported as they are: `Usb` for a
     * device with several functions, `UsbCdc` its serial functions.
     */
    export { Usb, UsbCdc } from "device/usb";

    /**
     * A USB device with a single serial function, started at once by the
     * constructor: the host sees a serial port (a COM port on Windows).
     *
     * It is a {@link Usb}: {@link Usb.close} stops the device, and the device
     * keeps the program running until then. Since it is started already,
     * calling {@link Usb.serial} or {@link Usb.start} on it throws "usb:
     * already started". As for any `Usb`, only one may exist at a time, and
     * the device outlives the program (taken over without re-enumeration by
     * the next program with the same descriptors).
     *
     * @example
     * import { UsbSerial } from "stdlib/usb";
     *
     * const port = new UsbSerial({ product: "Logger" });
     * setInterval(() => port.write(`${Date.now()}\r\n`), 1000);
     */
    export class UsbSerial extends Usb {
        /**
         * Describes the device, declares its serial function and starts it.
         *
         * @param descriptors as for `new Usb(descriptors)`; omitted: the
         *     defaults.
         * @throws what `new Usb()`, {@link Usb.serial} and {@link Usb.start}
         *     throw: `Error` "usb: busy: another Usb object exists" when
         *     another `Usb` exists, "usb: the hardware refused to start the
         *     device"; `TypeError` / `RangeError` for bad descriptors.
         */
        constructor(descriptors?: UsbDescriptors);

        /**
         * Sets the function called with each block of bytes received from
         * the host (a new `Uint8Array`), replacing the previous one. Anything
         * but a function is a `TypeError`.
         */
        onData(callback: (bytes: Uint8Array) => void): void;

        /**
         * Sets the function called with the new {@link UsbLineState} when the
         * host changes the control lines (opens or closes the port on its
         * side). Anything but a function is a `TypeError`.
         */
        onLineState(callback: (state: UsbLineState) => void): void;

        /**
         * Sends a string (UTF-8) or bytes to the host; what the host does not
         * take within about 50 ms is dropped without error (nobody there, or
         * not reading).
         *
         * @throws `Error` "usb: the device is not started" after
         *     {@link Usb.close}; `TypeError` for another type of data; a
         *     system error with code `EIO` when the device refused the bytes.
         */
        write(data: UsbData): void;

        /**
         * The control lines now (both `false` once closed).
         */
        lineState(): UsbLineState;
    }
}
