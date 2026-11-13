/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * Pulse trains of exact durations on a pin, sent by the hardware without the
 * CPU (the RMT peripheral of the ESP32). Durations are counted in ticks of
 * the resolution chosen when the channel is opened. Used by `stdlib/rgbled`
 * for WS2812 LEDs.
 *
 * Options objects are strict: an unknown field throws a TypeError (it
 * catches typos), a missing field takes its default.
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | — | ✓ | — | ✓ |
 *
 * @module device/rmt
 */
declare module "device/rmt" {
    /** Options of `new Rmt(pin, options)`. */
    export interface RmtOptions {
        /**
         * Ticks per second, 1000 to 80000000. Default 1000000 (1 tick =
         * 1 µs). Throws a RangeError out of that range.
         */
        resolution?: number;
    }

    /**
     * Bytes accepted by `rmt.sendBytes`: a Uint8Array, any other typed array
     * (its raw bytes) or an ArrayBuffer. Not a DataView, not an array of
     * numbers.
     */
    export type RmtBytes = Uint8Array | Uint8ClampedArray | Int8Array | Uint16Array | Int16Array | Uint32Array | Int32Array | Float16Array | Float32Array | Float64Array | BigInt64Array | BigUint64Array | ArrayBuffer;

    /** Options of `rmt.sendBytes(bytes, options)`. */
    export interface RmtBytesOptions {
        /** How a 0 bit is sent: `[high, low]`, ticks at 1 then at 0, each 1 to 32767. Required. */
        bit0: [number, number];
        /** How a 1 bit is sent: `[high, low]`, ticks at 1 then at 0, each 1 to 32767. Required. */
        bit1: [number, number];
        /**
         * Ticks the pin stays low after the bytes (the "reset" of a WS2812),
         * 0 to 100000000. Default 0: none.
         */
        reset?: number;
    }

    /**
     * A sending channel on one pin; the pin is low when idle.
     *
     * One send at a time: a second one while a promise is pending throws.
     * A pending send keeps the program running. At the end of the program
     * the channel is closed (a send in progress is dropped).
     *
     * Methods called after `close()` throw an `Error` ("rmt: closed").
     *
     * @example
     * import { Rmt } from "device/rmt";
     * const rmt = new Rmt(48, { resolution: 10_000_000 });   // 1 tick = 0.1 µs
     * await rmt.send([4, 1, 8, 0]);                          // 0.4 µs high, 0.8 µs low
     * await rmt.sendBytes(new Uint8Array([0, 255, 0]), { bit0: [4, 8], bit1: [8, 4], reset: 3000 });
     * rmt.close();
     */
    export class Rmt {
        /**
         * Opens a channel on the pin.
         *
         * Throws a RangeError for a pin outside 0..63 or a resolution out of
         * range, and an `Error` when no channel is free (4 on the ESP32-S3) or
         * the hardware refuses the pin or the resolution.
         * @param pin GPIO number.
         */
        constructor(pin: number, options?: RmtOptions);

        /**
         * Sends pulses `[ticks, level, ticks, level, ...]`: the pin stays at
         * `level` (0 or 1) for `ticks` (1 to 4294967295; long pulses are cut
         * into several by the driver). The pulses are copied: the array can
         * be changed at once.
         *
         * Throws a TypeError when `pulses` is not a non-empty array of an
         * even number of integers, an `Error` while a send is in progress or
         * when the hardware refuses it.
         * @returns A promise resolved when the pulses have been sent, rejected
         * if `close()` comes first.
         */
        send(pulses: number[]): Promise<void>;

        /**
         * Sends bytes, most significant bit first, each bit as `bit0` or
         * `bit1`, then keeps the pin low for `reset` ticks. The bytes are
         * copied. A typed array other than a Uint8Array is sent as its raw
         * bytes. (A plain array of numbers is NOT accepted here.)
         *
         * Throws a TypeError when `bytes` is empty or not bytes, or `bit0` /
         * `bit1` are not `[high, low]` tick counts (1 to 32767); an `Error`
         * while a send is in progress or when the hardware refuses it.
         * @returns A promise resolved when the bytes have been sent, rejected
         * if `close()` comes first.
         */
        sendBytes(bytes: RmtBytes, options: RmtBytesOptions): Promise<void>;

        /**
         * Closes the channel; a pending send is rejected with an `Error`
         * ("rmt: closed while sending"). The object can no longer be used;
         * closing twice does nothing.
         */
        close(): void;
    }
}
