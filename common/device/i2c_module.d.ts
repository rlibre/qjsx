/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * I2C master buses and the devices on them. A bus is a controller of the chip
 * on two pins; a device is a 7-bit address on a bus, with its own clock and
 * time limit. Transfers are made only through devices, as promises or as
 * `Sync` calls that block the JS thread.
 *
 * Order: the transfers of a bus run one at a time, in the order of the calls
 * (a `Sync` call first waits for the transfers already queued on its bus). A
 * sequence that must not be cut by another transfer (select a channel of a
 * switch, then read) is the program's business. At most 16 promises may be
 * pending per bus; a bus with pending promises keeps the program running.
 *
 * Transfer errors are `Error`s with a Node-like `code` (and `errno`):
 * `"ENXIO"` (nobody answered at the address, or a byte was refused: NACK),
 * `"ETIMEDOUT"` (not done within the time limit), `"EIO"` (anything else).
 *
 * Options objects are strict: an unknown field throws a TypeError (it
 * catches typos), a missing field takes its default.
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | — | ✓ | — | ✓ ¹ |
 *
 * ¹ The parts wired on the bus answer, any other address NACKs.
 *
 * @module device/i2c
 */
declare module "device/i2c" {
    /**
     * Bytes to write: a Uint8Array, any other typed array (its raw bytes), an
     * ArrayBuffer, or an array of numbers 0 to 255; 1 to 4096 bytes. Copied
     * when the call is made. Anything else throws a TypeError.
     */
    export type I2cBytes = Uint8Array | Uint8ClampedArray | Int8Array | Uint16Array | Int16Array | Uint32Array | Int32Array | Float16Array | Float32Array | Float64Array | BigInt64Array | BigUint64Array | ArrayBuffer | readonly number[];

    /** Options of `new I2c(port, options)`. */
    export interface I2cOptions {
        /** SDA pin, a GPIO number 0 to 255. Required (TypeError without it). */
        sda: number;
        /** SCL pin, a GPIO number 0 to 255. Required (TypeError without it). */
        scl: number;
        /**
         * Default clock of the devices of the bus, in Hz, 1000 to 5000000.
         * Default 100000.
         */
        frequency?: number;
        /**
         * Turns on the chip's internal pull-ups (too weak for 400 kHz:
         * prefer external resistors). Default false. Must be a boolean.
         */
        pullUp?: boolean;
        /**
         * Default time limit of a transfer, in milliseconds, 1 to 60000.
         * Default 100. Also the time limit of `probe` / `probeSync`.
         */
        timeout?: number;
    }

    /** Options of `bus.device(address, options)`. */
    export interface I2cDeviceOptions {
        /** Clock of this device, in Hz, 1000 to 5000000. Default: the bus's `frequency`. */
        frequency?: number;
        /** Time limit of its transfers, in milliseconds, 1 to 60000. Default: the bus's `timeout`. */
        timeout?: number;
    }

    /**
     * An I2C master bus.
     *
     * Closed by `close()` or at the end of the program. Methods called after
     * `close()` throw an `Error` ("i2c: bus closed").
     *
     * @example
     * import { I2c } from "device/i2c";
     * const bus = new I2c(0, { sda: 8, scl: 9, frequency: 400000 });
     * if (await bus.probe(0x48)) {
     *     const sensor = bus.device(0x48);
     *     await sensor.write([0x01, 0x60]);
     *     const word = await sensor.writeRead([0x00], 2);   // a register read
     * }
     * bus.close();
     */
    export class I2c {
        /**
         * Takes bus `port` of the chip on the pins given.
         *
         * Throws a TypeError without `options`, `sda` or `scl`; a RangeError
         * for a port outside 0..7, a bus the chip does not have or an option
         * out of range; an `Error` when the bus is open already or the pins
         * are refused.
         * @param port Number of the I2C controller, from 0.
         */
        constructor(port: number, options: I2cOptions);

        /**
         * The device at 7-bit `address` (0 to 127) on this bus. At most 16
         * devices per bus. The device keeps its bus object alive.
         *
         * Throws an `Error` when the driver refuses the device or the bus has
         * 16 devices already.
         */
        device(address: number, options?: I2cDeviceOptions): I2cDevice;

        /**
         * Whether something answers at 7-bit `address` (0 to 127), after the
         * transfers already queued.
         * @returns A promise of true (it answered) or false (NACK); rejected
         * only for another error (`code` `"ETIMEDOUT"` or `"EIO"`).
         */
        probe(address: number): Promise<boolean>;

        /**
         * `probe()` blocking the JS thread: true or false. Throws an `Error`
         * with `code` `"ETIMEDOUT"` or `"EIO"` for another error.
         */
        probeSync(address: number): boolean;

        /**
         * Frees a bus that a device holds stuck (clock pulses, then a stop),
         * after the transfers already queued. Blocks the JS thread.
         */
        reset(): void;

        /**
         * Closes the bus and its devices: the queued transfers are dropped,
         * the pending promises rejected with an `Error` ("i2c: bus closed").
         * The object can no longer be used; closing twice does nothing.
         */
        close(): void;
    }

    /**
     * A device on a bus, made by `bus.device(address)`; `new I2cDevice()`
     * throws a TypeError.
     *
     * Methods called after its `close()`, or after its bus is closed, throw
     * an `Error` ("i2c: device closed" / "i2c: bus closed").
     *
     * @example
     * const sensor = bus.device(0x76, { frequency: 100000 });
     * const id = sensor.writeReadSync([0xd0], 1)[0];
     */
    export class I2cDevice {
        /** Not constructible: use `bus.device(address)`. */
        private constructor();

        /**
         * Writes bytes.
         * @returns A promise resolved when they are written, rejected with an
         * `Error` with `code` `"ENXIO"`, `"ETIMEDOUT"` or `"EIO"`.
         */
        write(bytes: I2cBytes): Promise<void>;

        /**
         * Reads `length` bytes, 1 to 4096 (RangeError otherwise).
         * @returns A promise of the bytes read, rejected with an `Error` with
         * `code` `"ENXIO"`, `"ETIMEDOUT"` or `"EIO"`.
         */
        read(length: number): Promise<Uint8Array>;

        /**
         * Writes bytes then reads `length` bytes (1 to 4096) after a repeated
         * start, in one transfer: a register read.
         * @returns A promise of the bytes read, rejected with an `Error` with
         * `code` `"ENXIO"`, `"ETIMEDOUT"` or `"EIO"`.
         */
        writeRead(bytes: I2cBytes, length: number): Promise<Uint8Array>;

        /**
         * `write()` blocking the JS thread. Throws an `Error` with `code`
         * `"ENXIO"`, `"ETIMEDOUT"` or `"EIO"`.
         */
        writeSync(bytes: I2cBytes): void;

        /**
         * `read()` blocking the JS thread: the bytes read. Throws an `Error`
         * with `code` `"ENXIO"`, `"ETIMEDOUT"` or `"EIO"`.
         */
        readSync(length: number): Uint8Array;

        /**
         * `writeRead()` blocking the JS thread: the bytes read. Throws an
         * `Error` with `code` `"ENXIO"`, `"ETIMEDOUT"` or `"EIO"`.
         */
        writeReadSync(bytes: I2cBytes, length: number): Uint8Array;

        /**
         * Frees the device, after the transfers already queued on its bus.
         * The object can no longer be used; closing twice does nothing.
         */
        close(): void;
    }
}
