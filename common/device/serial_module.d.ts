/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * Serial ports: one class, {@link Serial}, for the serial ports of the PC and
 * for the UARTs of a board.
 *
 * - On the PC (`qjsx run`, an executable made by `qjsx build --executable`,
 *   the desktop agent `qjsx agent`): the ports of the system, by name
 *   (`"COM3"`, `"/dev/ttyUSB0"`). A program needs the permission to use them:
 *   `qjsx run --allow-serial[=ports]` (or `-A`), `"serial"` in the
 *   `desktop.permissions` of `qjsx.json` for an executable; refused:
 *   a system error with code `EACCES`. The desktop agent needs none.
 * - On a board (ESP32 today) and in the digital twin of a board: the UARTs of
 *   the chip, by number, on the pins the program gives. No permission. The
 *   UART of the agent's link (UART0 on the ESP32) cannot be opened. In the
 *   twin each UART is a TCP port of the PC and a section of the twin's page.
 *
 * The received bytes come through a callback ({@link Serial.onData}),
 * writing is synchronous ({@link Serial.write}). An open port keeps the
 * program running until it is closed; a port left open is freed at the end
 * of the program.
 *
 * @example
 * import { list, Serial } from "device/serial";
 *
 * console.log(list());                          // PC: ["COM3", ...]; board: [1, 2]
 * const port = new Serial("COM3", { baudRate: 115200 });                  // PC
 * // const port = new Serial(2, { baudRate: 115200, tx: 38, rx: 37 });   // board
 * port.onData(bytes => console.log(new TextDecoder().decode(bytes)));
 * port.onClose(() => console.log("closed"));
 * port.write("hello\r\n");
 * port.close();
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | ✓ ports of the system | ✓ UARTs, not UART0 | — | ✓ UARTs of the board |
 *
 * @module device/serial
 */
declare module "device/serial" {
    /**
     * What {@link Serial.write} accepts: a string (sent as UTF-8), an
     * `ArrayBuffer` (all its bytes) or a typed array (the bytes it views,
     * in memory order). A `DataView` or an array of
     * numbers is refused with a `TypeError`.
     */
    export type SerialData = string | ArrayBuffer | Uint8Array | Uint8ClampedArray | Int8Array | Uint16Array | Int16Array | Uint32Array | Int32Array | Float16Array | Float32Array | Float64Array | BigUint64Array | BigInt64Array;

    /**
     * Options of {@link Serial} common to the PC and the boards. Every field
     * is optional; an unknown field is a `TypeError` (catches typos), a value
     * out of its range a `RangeError`, a value of the wrong type (a number
     * with a fraction included) a `TypeError`.
     */
    export interface SerialOptions {
        /**
         * Speed in bits per second, an integer from 300 to 4,000,000.
         * Default: 115200. The driver may refuse a speed it cannot do
         * (system error `EINVAL`).
         */
        baudRate?: number;

        /** Data bits of a character. Default: 8. */
        dataBits?: 5 | 6 | 7 | 8;

        /** Parity bit. Default: `"none"`. */
        parity?: "none" | "even" | "odd";

        /** Stop bits. Default: 1. */
        stopBits?: 1 | 2;
    }

    /**
     * Options of a port of the PC (`new Serial("COM3", options)`). The pins
     * of a board (`tx`, `rx`, `rts`, `cts`) are a `TypeError` here.
     */
    export interface PcSerialOptions extends SerialOptions {
        /**
         * Hardware flow control: `"none"` (default) or `"rts-cts"`. Any other
         * string is a `TypeError`. PC only: on a board, give the `rts` and
         * `cts` pins instead.
         */
        flowControl?: "none" | "rts-cts";
    }

    /**
     * Options of a UART of a board (`new Serial(2, options)`): the GPIO
     * numbers of its signals (integers 0 to 255). A pin not given is not
     * wired. At least one of `tx` and `rx` must be given, and `rts` and `cts`
     * go together (they turn the hardware flow control on): otherwise a
     * `TypeError`. `flowControl` is a `TypeError` on a board.
     */
    export interface UartOptions extends SerialOptions {
        /** GPIO of the transmitted data (output). */
        tx?: number;

        /** GPIO of the received data (input). */
        rx?: number;

        /** GPIO of RTS (output); flow control, with `cts`. */
        rts?: number;

        /** GPIO of CTS (input); flow control, with `rts`. */
        cts?: number;
    }

    /**
     * The ports that can be opened now.
     *
     * - PC: their names, from the registry on Windows (`"COM3"`), the
     *   `/dev/ttyUSB*`, `/dev/ttyACM*` and `/dev/cu.*` devices elsewhere
     *   (full paths). Needs a serial permission (any port granted is enough);
     *   refused: system error with code `EACCES`.
     * - Board and twin: the numbers of the free UARTs (neither open nor used
     *   by the agent's link).
     *
     * @returns a new array, possibly empty.
     * @throws Error when the executable has no serial driver.
     */
    export function list(): string[] | number[];

    /**
     * An open serial port: a port of the PC or a UART of a board.
     *
     * Life cycle: the constructor opens the port and starts reading; from
     * then on the port keeps the program running until it is closed, by
     * {@link Serial.close} or by a read error (the device went away). A port
     * still open at the end of the program (`process.exit`, a device program
     * stopped) is closed and freed; the next program can open it at once.
     *
     * The received bytes are read by a thread of the runtime and delivered on
     * the program's thread through {@link Serial.onData}; bytes arriving
     * before a callback is set are dropped, but none are delivered before the
     * current synchronous code ends, so setting `onData` right after the
     * constructor loses nothing.
     *
     * @example
     * import { Serial } from "device/serial";
     *
     * const uart = new Serial(1, { baudRate: 9600, tx: 17, rx: 18 });
     * uart.onData(bytes => uart.write(bytes));      // echo
     */
    export class Serial {
        /**
         * Opens a port of the PC by its system name.
         *
         * @param port the name, such as `"COM3"` or `"/dev/ttyUSB0"`; not a
         *     string: `TypeError`.
         * @param options settings of the line; omitted or `undefined`: the
         *     defaults (`null` is a `TypeError`).
         * @throws a system error (an `Error` with a Node-like `code` and an
         *     `errno`): `ENOENT` no such port, `EBUSY` port in use (also by
         *     another program on Windows), `EINVAL` settings refused, `EACCES`
         *     not permitted (see the module), `EIO` or another code for any
         *     other failure; `TypeError` / `RangeError` for bad options.
         */
        constructor(port: string, options?: PcSerialOptions);
        /**
         * Opens a UART of a board (or of the digital twin of a board) by its
         * number, on the pins given in the options.
         *
         * @param port the number of the UART, a non-negative integer;
         *     otherwise a `TypeError`.
         * @param options settings and pins; `tx` or `rx` is required.
         * @throws a system error with a Node-like `code`: `ENOENT` the chip
         *     has no such UART, `EBUSY` the UART is open already or is the
         *     agent's link, `EINVAL` settings or pins refused by the driver,
         *     `EIO` the driver failed; `TypeError` / `RangeError` for bad
         *     options. A UART still being closed by a previous program is
         *     waited for (up to 300 ms) before `EBUSY`.
         */
        constructor(port: number, options: UartOptions);

        /**
         * Sets the function called with each block of bytes received,
         * replacing the previous one. The blocks follow the bursts of the
         * line (at most 1024 bytes each), not the messages of a protocol: a
         * message may come in several blocks, or several in one. On the
         * ESP32 a block ends after 10 characters of silence or 120 bytes.
         *
         * The callback gets a new `Uint8Array` (`this` is `undefined`); an
         * exception it throws is printed as uncaught and the port goes on.
         *
         * @param callback a function; anything else (including `undefined`
         *     or `null`) is a `TypeError`: a callback cannot be removed, only
         *     replaced.
         */
        onData(callback: (bytes: Uint8Array) => void): void;

        /**
         * Sets the function called once when the port has been closed: after
         * {@link Serial.close}, or after a read error (the device went
         * away). It gets no argument. Replaces the previous one; anything but
         * a function is a `TypeError`.
         */
        onClose(callback: () => void): void;

        /**
         * Writes bytes, synchronously: returns once the driver has taken all
         * of them (the program waits while the driver's buffer is full).
         *
         * @param data a string (sent as UTF-8), an `ArrayBuffer` or a typed
         *     array (the bytes it views).
         * @throws `Error` "port closed" once {@link Serial.close} was called
         *     or the port was closed by an error (even while
         *     {@link Serial.isOpen} is still `true`); `TypeError` for another
         *     type of data; a system error with code `EIO` when the write
         *     failed.
         */
        write(data: SerialData): void;

        /**
         * Closes the port. It returns at once: the reading stops and the port
         * is released within about 100 ms, then {@link Serial.onClose} is
         * called and {@link Serial.isOpen} turns `false`; from then on the
         * object no longer keeps the program running. Writing is refused as
         * soon as `close()` returns. Calling it again does nothing.
         *
         * On a board, a closed UART gives its pins back as plain GPIOs.
         */
        close(): void;

        /**
         * The port given to the constructor: its name on the PC (a string),
         * its UART number on a board (a number).
         */
        port(): string | number;

        /**
         * `true` from the constructor until the close has been handled
         * (after {@link Serial.close} or a read error, just before
         * {@link Serial.onClose} is called).
         */
        isOpen(): boolean;
    }
}
