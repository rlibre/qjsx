/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * Analog outputs: a DAC (digital to analog converter) drives a pin with a
 * voltage. Everything is synchronous.
 *
 * A code `c`, 0 to 2^bits - 1, gives `c / (2^bits - 1)` of the full scale,
 * the voltage of the highest code (`fullScale`, from the description of the
 * board: 3.3 V on the ESP32 boards). The ESP32's DAC has 8 bits: 256 codes,
 * a step of about 13 mV.
 *
 * Like `device/pwm`, and unlike `device/gpio`, `close()` AND the end of the
 * program stop the output and give the pin back.
 *
 * Errors:
 * - argument errors are a `TypeError` (wrong type, unknown option) or a
 *   `RangeError` (a number out of range, or not an integer where a code is
 *   expected: a fraction, NaN and Infinity are refused, never truncated),
 *   without `code`;
 * - the others are `Error`s with a Node-like `code` (and `errno`):
 *   `"EINVAL"` (no DAC on the pin), `"EBUSY"` (its DAC is open already),
 *   `"EBADF"` (the output is closed), `"EIO"` (the hardware refused it).
 *
 * Options objects are strict: an unknown field throws a TypeError (it
 * catches typos), a missing field takes its default.
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | i.MX RT1062 | Twin |
 * |---|---|---|---|---|
 * | — | ✓ ¹ | — | — | ✓ ² |
 *
 * ¹ Only the chips that have a DAC: the original ESP32 (GPIO25, GPIO26) and
 * the ESP32-S2 (GPIO17, GPIO18), 8 bits. Not the ESP32-S3: there, a
 * `device/pwm` output behind an RC filter makes a voltage.
 *
 * ² On the boards whose description has a `"dac"` section; the parts wired
 * on the pin read its voltage, the page shows it.
 *
 * @module device/dac
 */
declare module "device/dac" {
    /** Options of `new Dac(pin, options)`. */
    export interface DacOptions {
        /**
         * The first code driven, an integer 0 to 2^bits - 1. Default 0
         * (0 V).
         */
        value?: number;
    }

    /**
     * An analog output, by the GPIO number of its pin.
     *
     * Closed by `close()` or at the end of the program: the output stops and
     * the pin is given back (as a pin nobody configured). Writes after
     * `close()` throw an `Error` with `code` `"EBADF"`.
     *
     * @example
     * import { Dac } from "device/dac";
     * const out = new Dac(25);                 // GPIO25 of an ESP32, at 0 V
     * out.writeVoltage(1.2);                   // the nearest code: 93 of 255
     * console.log(out.value, "of", 2 ** out.bits - 1, "=", out.value / (2 ** out.bits - 1) * out.fullScale, "V");
     * out.write(255);                          // the full scale: 3.3 V
     * out.close();
     */
    export class Dac {
        /**
         * Opens the DAC of the pin and drives its first code.
         *
         * Throws a RangeError for a pin that is not an integer 0..255 or a
         * `value` that is not a code of the resolution; an `Error` with
         * `code` `"EINVAL"` when the pin has no DAC, `"EBUSY"` when its DAC
         * is open already.
         * @param pin GPIO number.
         */
        constructor(pin: number, options?: DacOptions);

        /**
         * Drives a raw code, an integer 0 to 2^bits - 1 (0 to 255 on 8 bits).
         * Throws a RangeError for anything else (257, 1.5, -1, NaN), an
         * `Error` with `code` `"EBADF"` when closed.
         */
        write(raw: number): void;

        /**
         * Drives a voltage, 0 to `fullScale` volts, rounded to the nearest
         * code (`value` says which). Throws a RangeError outside (or NaN), an
         * `Error` with `code` `"EBADF"` when closed.
         */
        writeVoltage(volts: number): void;

        /** Stops the output and gives the pin back. Closing twice does nothing. */
        close(): void;

        /** The resolution, in bits: codes 0 to 2^bits - 1 (8 on the ESP32). */
        readonly bits: number;

        /**
         * The voltage of the highest code, in volts (from the description of
         * the board: 3.3 on the ESP32 boards).
         */
        readonly fullScale: number;

        /** The code written last (kept after `close()`). */
        readonly value: number;
    }
}
