/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * The board as a USB device for a host (a PC, a tablet), on the chip's
 * native USB port (not the USB-serial bridge of the agent's link).
 *
 * A composite device: {@link Usb} describes it, its functions are declared
 * with {@link Usb.serial} (serial functions, CDC ACM: at most 2), then
 * {@link Usb.start} presents them all at once to the host. Only one `Usb`
 * object exists at a time.
 *
 * The device outlives the program: at the end of a program it stays started
 * (the bytes received meanwhile are dropped). The next program that starts a
 * device with the same descriptors and the same number of functions takes it
 * over without the host seeing it go away; other descriptors restart it.
 * Only {@link Usb.close} stops it.
 *
 * The common case, one serial function, is {@link "stdlib/usb".UsbSerial}.
 *
 * @example
 * import { Usb } from "device/usb";
 *
 * const usb = new Usb({ manufacturer: "Acme", product: "Gadget", serialNumber: "v2.0.0" });
 * const port = usb.serial();                  // declared before start()
 * port.onData(bytes => port.write(bytes));    // echo
 * port.onLineState(({ dtr, rts }) => console.log("DTR", dtr, "RTS", rts));
 * usb.start();                                // the host sees the device
 * port.write("hello\r\n");
 * // ... later
 * usb.close();                                // the host sees it go away
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | — | ✓ ¹ | — | ✓ ² |
 *
 * ¹ The chips with USB OTG: ESP32-S2, S3, P4.
 *
 * ² Each serial function is a TCP port of the PC (chosen on the twin's page, or `qjsx agent --usb-port`).
 *
 * @module device/usb
 */
declare module "device/usb" {
    /**
     * What {@link UsbCdc.write} accepts: a string (sent as UTF-8), an
     * `ArrayBuffer` (all its bytes) or a typed array (the bytes it views, in
     * memory order). A `DataView` or an array of numbers
     * is refused with a `TypeError`.
     */
    export type UsbData = string | ArrayBuffer | Uint8Array | Uint8ClampedArray | Int8Array | Uint16Array | Int16Array | Uint32Array | Int32Array | Float16Array | Float32Array | Float64Array | BigUint64Array | BigInt64Array;

    /**
     * What the host sees of the device: the options of `new Usb(options)`.
     * Every field is optional; an unknown field is a `TypeError` (catches
     * typos).
     *
     * The texts are printable ASCII (codes 0x20 to 0x7E), 31 characters at
     * most, else a `RangeError`; a value that is not a string is a
     * `TypeError`.
     */
    export interface UsbDescriptors {
        /** Manufacturer string. Default: `"qjsx"`. */
        manufacturer?: string;

        /** Product string. Default: `"qjsx device"`. */
        product?: string;

        /** Serial number string. Default: none (empty). */
        serialNumber?: string;

        /**
         * USB vendor ID, an integer from 0 to 0xFFFF. Default 0: the default
         * of the chip's vendor (0x303A on the ESP32). Out of range: a
         * `RangeError`; not an integer: a `TypeError`.
         */
        vendorId?: number;

        /**
         * USB product ID, an integer from 0 to 0xFFFF. Default 0: the default
         * of the chip's vendor (0x4001 on the ESP32). Out of range: a
         * `RangeError`; not an integer: a `TypeError`.
         */
        productId?: number;
    }

    /**
     * The control lines of a serial function, as the host sets them.
     */
    export interface UsbLineState {
        /**
         * DTR, raised by most programs when they open the port on the host
         * side; not all do (a host may read without raising it).
         */
        dtr: boolean;

        /**
         * RTS. On Windows it stays raised after the port is closed, so it
         * does not tell whether the port is open.
         */
        rts: boolean;
    }

    /**
     * A USB device: its descriptors, its functions, and its life on the bus.
     *
     * Only one `Usb` object at a time: the next `new Usb()` throws until this
     * one is closed (or garbage collected, or the program ended). A started
     * device keeps the program running until {@link Usb.close}.
     *
     * The class can be extended from JS (as {@link "stdlib/usb".UsbSerial}
     * does).
     *
     * @example
     * import { Usb } from "device/usb";
     *
     * const usb = new Usb({ product: "Two ports" });
     * const data = usb.serial();
     * const debug = usb.serial();
     * usb.start();
     * debug.write("ready\r\n");
     */
    export class Usb {
        /**
         * Describes a device; nothing happens on the bus until
         * {@link Usb.start}.
         *
         * @param descriptors what the host will see; omitted or `undefined`:
         *     the defaults (`null` is a `TypeError`).
         * @throws `Error` (no `code`) "usb: busy: another Usb object exists"
         *     when another `Usb` object is neither closed nor collected;
         *     `TypeError` / `RangeError` for bad descriptors.
         */
        constructor(descriptors?: UsbDescriptors);

        /**
         * Declares a serial function (USB CDC ACM: the host sees a serial
         * port, a COM port on Windows) and returns it. Only before
         * {@link Usb.start}; the functions get their order of declaration.
         *
         * @returns the new function; this object keeps it alive.
         * @throws `Error` "usb: closed" after {@link Usb.close}, "usb: already
         *     started" after {@link Usb.start}; `RangeError` for a third
         *     serial function (at most 2).
         */
        serial(): UsbCdc;

        /**
         * Presents the device and its functions to the host:
         * 1. a device with the same descriptors and the same number of
         *    functions already started (by a previous program) is taken over:
         *    the host sees nothing;
         * 2. otherwise the device started before (if any) is stopped and this
         *    one started: the host sees the old one go away and this one
         *    come;
         * 3. the events of the device (bytes received, line states) now go to
         *    the functions of this object, which keeps the program running
         *    until {@link Usb.close}.
         *
         * @throws `Error` "usb: closed", "usb: already started", "usb: no
         *     function" (declare one with {@link Usb.serial} first), or "usb:
         *     the hardware refused to start the device". None of them has a
         *     `code`.
         */
        start(): void;

        /**
         * Stops the device if this object started it (the host sees it go
         * away; the program is no longer kept running by it) and frees the
         * place for another `Usb` object. This object can no longer be used:
         * {@link Usb.serial} and {@link Usb.start} throw, the writes of its
         * functions throw. Calling it again does nothing.
         *
         * An object that was never started does not stop a device left
         * started by a previous program.
         */
        close(): void;
    }

    /**
     * A serial function (USB CDC ACM) of a {@link Usb} device: on the host
     * it is a serial port. Made by {@link Usb.serial}, never by `new`.
     *
     * Its events (bytes, line states) come only while its `Usb` object is
     * the started device; set the callbacks before {@link Usb.start}.
     *
     * @example
     * const port = usb.serial();
     * port.onData(bytes => console.log("received", bytes.length, "bytes"));
     * usb.start();
     */
    export class UsbCdc {
        /**
         * Not constructible from JS: `new UsbCdc()` is a `TypeError`. Use
         * {@link Usb.serial}.
         */
        private constructor();

        /**
         * Sets the function called with each block of bytes received from
         * the host (a new `Uint8Array`; `this` is `undefined`), replacing the
         * previous one. An exception it throws is printed as uncaught and the
         * device goes on. Anything but a function is a `TypeError`.
         */
        onData(callback: (bytes: Uint8Array) => void): void;

        /**
         * Sets the function called when the host changes the control lines
         * (opens or closes the port on its side), with the new
         * {@link UsbLineState}; replaces the previous one. Anything but a
         * function is a `TypeError`.
         */
        onLineState(callback: (state: UsbLineState) => void): void;

        /**
         * Sends bytes to the host. The bytes are copied before it returns;
         * what the host does not take within about 50 ms is dropped without
         * error (all of it when no host is there, or nothing reads the port),
         * as on a UART with nobody at the other end. DTR is not a condition.
         *
         * @param data a string (sent as UTF-8), an `ArrayBuffer` or a typed
         *     array (the bytes it views).
         * @throws `Error` (no `code`) "usb: the device is not started" when
         *     its `Usb` object is not started or was closed; `TypeError` for
         *     another type of data; a system error with code `EIO` when the
         *     device refused the bytes.
         */
        write(data: UsbData): void;

        /**
         * The control lines now, as a new object; both `false` while its
         * `Usb` object is not the started device.
         */
        lineState(): UsbLineState;
    }
}
