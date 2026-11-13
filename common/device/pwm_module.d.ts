/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * PWM outputs: a square signal of a frequency and a duty cycle (the share of
 * each period the output is active) on a pin. Everything is synchronous.
 *
 * The resolution of the duty cycle is the highest the frequency allows; the
 * hardware rounds the frequency and the duty cycle, and the properties of a
 * `Pwm` say what it really does. Outputs share timers, hence frequencies (on
 * the ESP32: 8 outputs, 4 timers, 14 bits at most; 14 bits at 1 kHz, 11 at
 * 20 kHz, 1 at 40 MHz; under about 5 Hz out of reach): a frequency no timer
 * can take is refused, the other outputs are never changed.
 *
 * Unlike `device/gpio`, `close()` AND the end of the program stop the output:
 * the pin stays driven at its idle level (0, or 1 when inverted).
 *
 * Options objects are strict: an unknown field throws a TypeError (it
 * catches typos), a missing field takes its default.
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | — | ✓ LEDC | — | ✓ ¹ |
 *
 * ¹ Rounds and refuses as the ESP32.
 *
 * @module device/pwm
 */
declare module "device/pwm" {
    /** Options of `new Pwm(pin, options)`. */
    export interface PwmOptions {
        /**
         * Frequency in Hz, an integer 1 to 40000000. Default 1000. Throws a
         * RangeError out of that range.
         */
        frequency?: number;
        /** Duty cycle, 0 to 1 (0: always idle, 1: always active). Default 0. */
        duty?: number;
        /**
         * Active low: the pin is 1 at rest (a LED wired to the supply).
         * Default false. Must be a boolean.
         */
        invert?: boolean;
    }

    /**
     * A PWM output on a pin, by its GPIO number.
     *
     * Setters called after `close()` throw an `Error` ("pwm: closed").
     *
     * @example
     * import { Pwm } from "device/pwm";
     * const servo = new Pwm(5, { frequency: 50 });
     * servo.setPulseWidth(1500);      // 1.5 ms per 20 ms period: the middle
     * const led = new Pwm(6, { frequency: 1000, invert: true });
     * led.setDuty(0.25);
     * console.log(led.duty, led.frequency, led.bits);
     */
    export class Pwm {
        /**
         * Starts the output.
         *
         * Throws a RangeError for a pin outside 0..63, a frequency out of
         * range or a duty cycle outside 0 to 1; an `Error` with `code`
         * `"EINVAL"` (no output on the pin, a frequency out of reach of the
         * hardware) or `"EBUSY"` (the pin is a PWM output already, no output
         * or no timer free for that frequency).
         * @param pin GPIO number.
         */
        constructor(pin: number, options?: PwmOptions);

        /**
         * Another duty cycle, 0 to 1, from the next period (the phase is
         * kept). Throws a RangeError outside 0 to 1 (NaN included).
         */
        setDuty(duty: number): void;

        /**
         * Another frequency in Hz, an integer 1 to 40000000 (RangeError
         * otherwise); the duty cycle asked last is kept, on the resolution of
         * the new frequency. Throws an `Error` with `code` `"EINVAL"` (out of
         * reach) or `"EBUSY"` (no timer free for it: the output keeps its
         * frequency).
         */
        setFrequency(frequency: number): void;

        /**
         * The time the output is active in each period, in microseconds (for
         * a servo), set as a duty cycle at the real frequency
         * (`microseconds * 1e-6 * this.frequency`). Throws a RangeError when
         * negative or longer than one period.
         */
        setPulseWidth(microseconds: number): void;

        /**
         * Stops the output: the pin stays driven at its idle level, the timer
         * is freed when no other output uses it. Closing twice does nothing.
         */
        close(): void;

        /**
         * The duty cycle the hardware makes, 0 to 1, rounded to a step of the
         * resolution. Updated by each call. Writable, but writing it changes
         * nothing in the output.
         */
        duty: number;

        /**
         * The frequency the hardware makes, in Hz (rounded by its divider).
         * Updated by each call. Writable, but writing it changes nothing in
         * the output (it does change what `setPulseWidth` computes).
         */
        frequency: number;

        /**
         * The resolution of the duty cycle, in bits: 2^bits steps per period.
         * Updated by each call. Writable, but writing it changes nothing.
         */
        bits: number;
    }
}
