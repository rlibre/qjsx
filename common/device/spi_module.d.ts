/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * SPI master buses and the devices on them. A bus is a controller of the chip
 * on its clock and data pins; a device is a chip select on a bus, with its
 * own clock, mode, bit order and optional data / command (DC) pin, set by the
 * driver just before each segment (a display driver needs no `Gpio` for it).
 * Transfers are made only through devices, as promises or as `Sync` calls
 * that block the JS thread.
 *
 * A transaction is a list of segments run with the chip select held from the
 * first segment to the last: bytes written, bytes read (the device's `fill`
 * byte is sent meanwhile), bytes exchanged (full duplex) or a short delay.
 * Segments of any size are cut by the driver in pieces of at most the bus's
 * `maxTransfer` bytes, the chip select kept.
 *
 * Order: the transactions of a bus run one at a time, in the order of the
 * calls (a `Sync` call first waits for the transactions already queued on its
 * bus). At most 16 transactions may be pending per bus; a bus with pending
 * promises keeps the program running. The promise forms copy the bytes they
 * send; the `Sync` forms send them in place (better for a large frame on a
 * board with little RAM).
 *
 * Errors:
 * - a method that returns a promise NEVER throws: every error, argument
 *   errors included, rejects its promise;
 * - argument errors are a `TypeError` (wrong type, unknown option) or a
 *   `RangeError` (a number out of range, or not an integer: a fraction, NaN
 *   and Infinity are refused, never truncated), without `code`;
 * - the others are `Error`s with a Node-like `code` (and `errno`): `"EBADF"`
 *   (the bus or the device is closed), `"EAGAIN"` (16 transactions pending on
 *   the bus already), `"ECANCELED"` (a pending transaction dropped by
 *   `close()`), `"EIO"` (the hardware failed); opening a bus or adding a
 *   device: `"ENOENT"` (no such port), `"EBUSY"` (taken), `"EINVAL"` (a pin
 *   the chip cannot use).
 *
 * Options objects are strict: an unknown field throws a TypeError (it
 * catches typos), a missing field takes its default.
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | i.MX RT1062 | Twin |
 * |---|---|---|---|---|
 * | — | ✓ | — | — | ✓ ¹ |
 *
 * ¹ The parts wired on the bus answer; the page shows the transactions.
 *
 * @module device/spi
 */
declare module "device/spi" {
    /**
     * Bytes to send: a Uint8Array, any other typed array (its raw bytes), an
     * ArrayBuffer, or an array of numbers 0 to 255 (a number that is not an
     * integer 0..255 is a RangeError); 1 to 1048576 bytes (an empty one is a
     * RangeError). Anything else is a TypeError.
     */
    export type SpiBytes = Uint8Array | Uint8ClampedArray | Int8Array | Uint16Array | Int16Array | Uint32Array | Int32Array | Float16Array | Float32Array | Float64Array | BigInt64Array | BigUint64Array | ArrayBuffer | readonly number[];

    /** Options of `new Spi(port, options)`. */
    export interface SpiOptions {
        /** Clock pin, a GPIO number 0 to 255. Required (TypeError without it). */
        sclk: number;
        /** Data out pin (MOSI), a GPIO number 0 to 255. Not wired when absent. */
        mosi?: number;
        /**
         * Data in pin (MISO), a GPIO number 0 to 255. Not wired when absent.
         * At least one of `mosi` and `miso` is required (TypeError).
         */
        miso?: number;
        /**
         * Bytes of one transfer of the hardware (DMA), 1 to 32768. Default
         * 4096. Larger segments are cut in pieces of this size.
         */
        maxTransfer?: number;
    }

    /** Options of `bus.device(options)`. */
    export interface SpiDeviceOptions {
        /**
         * Chip select pin, a GPIO number 0 to 255. When absent the program
         * drives it itself (a single device on the bus, or a `Gpio`).
         */
        cs?: number;
        /**
         * Clock asked for, in Hz, 1000 to 80000000. Default 1000000. The
         * hardware rounds it: see `SpiDevice.frequency`. On the ESP32, above
         * about 26 MHz on pins routed through the GPIO matrix, reads may be
         * wrong (writes are right).
         */
        frequency?: number;
        /**
         * SPI mode 0 to 3: bit 1 = CPOL (clock idle level), bit 0 = CPHA
         * (data sampled on the second edge). Default 0.
         */
        mode?: number;
        /** Order of the bits of each byte: `"msb"` first (default) or `"lsb"` first. */
        bitOrder?: "msb" | "lsb";
        /** The chip select is active at level 1 (most chips: 0). Default false. Must be a boolean. */
        csActiveHigh?: boolean;
        /**
         * Data / command pin, a GPIO number 0 to 255: set to each segment's
         * `dc` level (default 1, data) just before its bytes; `command()`
         * sends with it at 0. None when absent.
         */
        dc?: number;
        /** Byte sent while reading, 0 to 255. Default 0x00 (some chips want 0xff). */
        fill?: number;
    }

    /**
     * One segment of a transaction: exactly one of `write`, `read`,
     * `transfer`, `delay` (a TypeError otherwise), plus `dc`.
     */
    export interface SpiSegment {
        /** Bytes to send. */
        write?: SpiBytes;
        /** Number of bytes to read, 1 to 1048576 (the device's `fill` byte is sent meanwhile). */
        read?: number;
        /** Bytes to send while receiving as many (full duplex). */
        transfer?: SpiBytes;
        /**
         * Microseconds to wait, the chip select kept, 0 to 1000 (RangeError
         * above: a longer wait is an `await` between two transactions).
         */
        delay?: number;
        /**
         * Level of the DC pin during the segment: 0 (command) or 1 (data,
         * the default). A TypeError when the device has no `dc` pin.
         */
        dc?: 0 | 1;
    }

    /**
     * An SPI master bus.
     *
     * Closed by `close()` or at the end of the program. Methods called after
     * `close()` throw an `Error` with `code` `"EBADF"`.
     *
     * @example
     * import { Spi } from "device/spi";
     * const bus = new Spi(2, { sclk: 12, mosi: 11, miso: 13 });
     * const flash = bus.device({ cs: 10, frequency: 10000000 });
     * const id = await flash.writeRead([0x9f], 3);          // JEDEC id
     * const [status] = await flash.transaction([{ write: [0x05] }, { read: 1 }]);
     * bus.close();
     */
    export class Spi {
        /**
         * Takes controller `port` of the chip on the pins given. On the
         * ESP32: 2 (SPI2) or 3 (SPI3); 0 and 1 hold the flash.
         *
         * Throws a TypeError without `options`, `sclk` or a data pin; a
         * RangeError for a port outside 0..255 or an option out of range or
         * not an integer; an `Error` with `code` `"ENOENT"` (no such port),
         * `"EBUSY"` (open already, or kept for the flash), `"EINVAL"` (a pin
         * the chip cannot use) or `"EIO"` (the driver failed).
         * @param port Number of the SPI controller.
         */
        constructor(port: number, options: SpiOptions);

        /**
         * A device on this bus. At most 16 devices per bus (the hardware may
         * take fewer: 6 on the ESP32). The device keeps its bus object alive.
         *
         * Throws a TypeError / RangeError for invalid options; an `Error`
         * with `code` `"EBADF"` (bus closed), `"EBUSY"` (no device left),
         * `"EINVAL"` (a `cs` or `dc` pin the chip cannot drive) or `"EIO"`
         * (the driver refused the device, e.g. its clock).
         */
        device(options?: SpiDeviceOptions): SpiDevice;

        /**
         * Closes the bus and its devices, after the transaction in progress
         * (its promise settles with its result): the queued transactions are
         * dropped, their promises rejected with `code` `"ECANCELED"`. The
         * object can no longer be used; closing twice does nothing.
         */
        close(): void;
    }

    /**
     * A device on a bus, made by `bus.device(options)`; `new SpiDevice()`
     * throws a TypeError.
     *
     * After its `close()`, or after its bus is closed, the `Sync` methods
     * throw and the others reject with an `Error` with `code` `"EBADF"`.
     *
     * @example
     * // a display: a command, its parameters, then pixels, under one chip select
     * const lcd = bus.device({ cs: 10, dc: 9, frequency: 20000000 });
     * await lcd.transaction([
     *     { write: [0x2c], dc: 0 },          // RAMWR, a command
     *     { write: pixels },                 // the pixels, data
     * ]);
     */
    export class SpiDevice {
        /** Not constructible: use `bus.device(options)`. */
        private constructor();

        /**
         * The clock the hardware makes, in Hz (the frequency asked for,
         * rounded by its divider). Set by `bus.device()`.
         */
        readonly frequency: number;

        /**
         * Runs `segments` (1 to 1024 of them) with the chip select held from
         * the first to the last.
         * @returns A promise of one Uint8Array per `read` / `transfer`
         * segment, in order (an empty array when none). Never throws: it
         * rejects with a TypeError / RangeError for invalid segments, or an
         * `Error` with `code` `"EBADF"`, `"EAGAIN"`, `"ECANCELED"` or
         * `"EIO"`.
         */
        transaction(segments: readonly SpiSegment[]): Promise<Uint8Array[]>;

        /**
         * `transaction()` blocking the JS thread: one Uint8Array per `read` /
         * `transfer` segment. Throws a TypeError / RangeError for invalid
         * segments, or an `Error` with `code` `"EBADF"` or `"EIO"`.
         */
        transactionSync(segments: readonly SpiSegment[]): Uint8Array[];

        /**
         * Writes bytes (DC at 1).
         * @returns A promise resolved when they are sent. Never throws: it
         * rejects with a TypeError / RangeError for invalid bytes, or an
         * `Error` with `code` `"EBADF"`, `"EAGAIN"`, `"ECANCELED"` or
         * `"EIO"`.
         */
        write(bytes: SpiBytes): Promise<void>;

        /**
         * `write()` blocking the JS thread. Throws a TypeError / RangeError
         * for invalid bytes, or an `Error` with `code` `"EBADF"` or `"EIO"`.
         */
        writeSync(bytes: SpiBytes): void;

        /**
         * Reads `length` bytes, 1 to 1048576, sending the `fill` byte.
         * @returns A promise of the bytes read. Never throws: it rejects with
         * a TypeError / RangeError for an invalid length, or an `Error` with
         * `code` `"EBADF"`, `"EAGAIN"`, `"ECANCELED"` or `"EIO"`.
         */
        read(length: number): Promise<Uint8Array>;

        /**
         * `read()` blocking the JS thread: the bytes read. Throws a TypeError
         * / RangeError for an invalid length, or an `Error` with `code`
         * `"EBADF"` or `"EIO"`.
         */
        readSync(length: number): Uint8Array;

        /**
         * Sends bytes while receiving as many (full duplex).
         * @returns A promise of the bytes received. Never throws: it rejects
         * with a TypeError / RangeError for invalid bytes, or an `Error` with
         * `code` `"EBADF"`, `"EAGAIN"`, `"ECANCELED"` or `"EIO"`.
         */
        transfer(bytes: SpiBytes): Promise<Uint8Array>;

        /**
         * `transfer()` blocking the JS thread: the bytes received. Throws a
         * TypeError / RangeError for invalid bytes, or an `Error` with `code`
         * `"EBADF"` or `"EIO"`.
         */
        transferSync(bytes: SpiBytes): Uint8Array;

        /**
         * Writes bytes then reads `length` bytes (1 to 1048576) under one
         * chip select: a register read.
         * @returns A promise of the bytes read. Never throws: it rejects with
         * a TypeError / RangeError for invalid arguments, or an `Error` with
         * `code` `"EBADF"`, `"EAGAIN"`, `"ECANCELED"` or `"EIO"`.
         */
        writeRead(bytes: SpiBytes, length: number): Promise<Uint8Array>;

        /**
         * `writeRead()` blocking the JS thread: the bytes read. Throws a
         * TypeError / RangeError for invalid arguments, or an `Error` with
         * `code` `"EBADF"` or `"EIO"`.
         */
        writeReadSync(bytes: SpiBytes, length: number): Uint8Array;

        /**
         * Writes bytes with the DC pin at 0: a command of a display.
         * @returns A promise resolved when they are sent. Never throws: it
         * rejects with a TypeError when the device has no `dc` pin or for
         * invalid bytes, a RangeError for invalid bytes, or an `Error` with
         * `code` `"EBADF"`, `"EAGAIN"`, `"ECANCELED"` or `"EIO"`.
         */
        command(bytes: SpiBytes): Promise<void>;

        /**
         * `command()` blocking the JS thread. Throws a TypeError when the
         * device has no `dc` pin or for invalid bytes, a RangeError for
         * invalid bytes, or an `Error` with `code` `"EBADF"` or `"EIO"`.
         */
        commandSync(bytes: SpiBytes): void;

        /**
         * Closes the device: its queued transactions are dropped (their
         * promises rejected with `code` `"ECANCELED"`), the one in progress
         * ends; it then leaves its bus. The object can no longer be used;
         * closing twice does nothing.
         */
        close(): void;
    }
}
