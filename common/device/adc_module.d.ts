/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * Analog inputs: raw conversions and calibrated voltages. A read is
 * synchronous and short (a few microseconds).
 *
 * Options objects are strict: an unknown field throws a TypeError (it
 * catches typos), a missing field takes its default.
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | — | ✓ ADC1 only ¹ | — | ✓ |
 *
 * ¹ ADC2 does not work while the WiFi is on.
 *
 * @module device/adc
 */
declare module "device/adc" {
    /** Options of `new Adc(pin, options)`. */
    export interface AdcOptions {
        /**
         * The highest voltage expected on the pin, in volts, 0.1 to 40.
         * Default 3.3. The converter's setting that covers it is chosen
         * (on the ESP32 the attenuation: about 0.95, 1.25, 1.75 or 3.1 V;
         * above 3.1 V the reads saturate, above 3.3 V the range is refused).
         */
        range?: number;
    }

    /**
     * An analog input, by the GPIO number of its pin.
     *
     * Closed by `close()` or at the end of the program. Methods called after
     * `close()` throw an `Error` ("adc: closed").
     *
     * @example
     * import { Adc } from "device/adc";
     * const pot = new Adc(4, { range: 3.3 });
     * console.log(pot.read(), "of", 2 ** pot.bits - 1, "=", pot.readVoltage(), "V");
     * pot.close();
     */
    export class Adc {
        /**
         * Opens the analog input of the pin for the range asked.
         *
         * Throws a RangeError for a pin outside 0..63 or a range outside 0.1
         * to 40 volts; an `Error` with `code` `"EINVAL"` when the pin has no
         * analog input (or it is on ADC2 of the ESP32) or the converter
         * cannot read that range, `"EBUSY"` when the input is open already.
         * @param pin GPIO number.
         */
        constructor(pin: number, options?: AdcOptions);

        /**
         * A raw conversion, 0 to 2^bits - 1 (0 to 4095 on 12 bits). Throws
         * an `Error` with `code` `"EIO"` if the conversion fails.
         */
        read(): number;

        /**
         * A conversion in volts, corrected by the calibration of the chip.
         * Throws an `Error` with `code` `"EIO"` if the conversion fails.
         */
        readVoltage(): number;

        /** Closes the input; reads throw after it. Closing twice does nothing. */
        close(): void;

        /**
         * The resolution of the raw values, in bits (12 on the ESP32), set
         * when the input is opened. The property is writable, but writing it
         * changes nothing in the converter.
         */
        bits: number;
    }
}
