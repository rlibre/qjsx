/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * Digital pins: inputs, push-pull outputs, open-drain outputs, change
 * callbacks and waits for a level.
 *
 * Pins are GPIO numbers of the chip. Nothing is checked beyond their range: a
 * pin of the flash, of the PSRAM or of the console UART can be taken, the
 * program is responsible. A pin is NOT reset at the end of the program (an
 * output keeps its level); only `close()` puts it back as at boot.
 *
 * Options objects are strict: an unknown field throws a TypeError (it
 * catches typos), a missing field takes its default.
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | — | ✓ | ✓ | ✓ |
 *
 * @module device/gpio
 */
declare module "device/gpio" {
    /**
     * Direction of a pin:
     * - `"input"`: read only;
     * - `"output"`: push-pull output, starting at level 0, its level readable;
     * - `"openDrain"`: drives low or lets go (needs a pull-up), readable.
     */
    export type GpioMode = "input" | "output" | "openDrain";

    /** Internal pull resistor of a pin. */
    export type GpioPull = "none" | "up" | "down";

    /**
     * Changes that call an `onChange` callback: `"rising"` (to 1),
     * `"falling"` (to 0) or `"both"`.
     */
    export type GpioEdge = "rising" | "falling" | "both";

    /** Options of `new Gpio(pin, options)`. */
    export interface GpioOptions {
        /** Direction of the pin. Default `"input"`. */
        mode?: GpioMode;
        /** Internal pull resistor. Default `"none"`. */
        pull?: GpioPull;
    }

    /** Options of `gpio.onChange(callback, options)`. */
    export interface GpioChangeOptions {
        /** Which changes call the callback. Default `"both"`. */
        edge?: GpioEdge;
        /**
         * Time in milliseconds the level must stay the same before a change
         * is reported, 0 to 60000. Default 0: every change is reported.
         * Throws a RangeError out of that range.
         */
        debounce?: number;
    }

    /** Options of `gpio.waitFor(level, options)`. */
    export interface GpioWaitOptions {
        /**
         * Time limit in milliseconds, 0 to 86400000 (one day). Absent: no
         * limit. Throws a RangeError out of that range.
         */
        timeout?: number;
    }

    /**
     * A digital pin, by its GPIO number.
     *
     * An object with an `onChange` callback or a pending `waitFor` keeps the
     * program running until `close()`. An object collected without `close()`
     * stops watching its pin but leaves the pin as it is.
     *
     * Methods called after `close()` throw an `Error` ("gpio: closed").
     *
     * @example
     * import { Gpio } from "device/gpio";
     * const led = new Gpio(5, { mode: "output" });
     * const button = new Gpio(0, { mode: "input", pull: "up" });
     * button.onChange(level => led.write(level === 0), { edge: "both", debounce: 20 });
     */
    export class Gpio {
        /**
         * Configures the pin (an output starts at level 0).
         *
         * Throws a RangeError for a pin outside 0..63 or a pin the chip does
         * not have, a TypeError for a bad option, and an `Error` when the chip
         * refuses the configuration (an input-only pin as an output...).
         * @param pin GPIO number.
         * @param options Direction and pull; default an input without pull.
         */
        constructor(pin: number, options?: GpioOptions);

        /**
         * The level of the pin, outputs included.
         * @returns 0 (low) or 1 (high).
         */
        read(): 0 | 1;

        /**
         * Sets the level of an output. Any value is accepted and taken as a
         * condition: `1`, `true` or any truthy value gives 1, `0`, `false` or
         * any falsy value gives 0. As fast as the chip allows (made to be
         * called in loops).
         */
        write(level: number | boolean): void;

        /** Inverts what an output drives (its output latch, not the level read on the pin). */
        toggle(): void;

        /**
         * Calls `callback(level)` at each change of the level that `edge`
         * selects, once the level has stayed the same for `debounce`
         * milliseconds. A level equal to the last one reported is not
         * reported again. Replaces the previous callback (there is no way to
         * remove it but `close()`).
         *
         * Keeps the program running until `close()`. The callback runs on the
         * JS thread, from the event loop. Throws an `Error` if the pin cannot
         * be watched.
         * @param callback Receives the new level, 0 or 1.
         */
        onChange(callback: (level: 0 | 1) => void, options?: GpioChangeOptions): void;

        /**
         * Waits until the pin reads `level`: the promise resolves at once if
         * it does already. Only one wait at a time: a second call while one
         * is pending throws an `Error`.
         *
         * The promise is rejected with an `Error` after `timeout`
         * milliseconds ("gpio: waitFor timed out", no `code`) or by
         * `close()`. A pending wait keeps the program running.
         * @param level 0 or 1 (a RangeError otherwise, a TypeError for a boolean).
         */
        waitFor(level: 0 | 1, options?: GpioWaitOptions): Promise<void>;

        /**
         * Stops watching, rejects a pending `waitFor`, puts the pin back as
         * at boot (input, no pull). The object can no longer be used; closing
         * twice does nothing.
         */
        close(): void;
    }
}
