/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * Strips of addressable RGB LEDs (WS2812 and the like), from the standard
 * library (written in JS on top of the native module `device/rmt`, which
 * sends their bits with exact timings).
 *
 * The colors are set in memory ({@link RgbLed.set}, {@link RgbLed.fill},
 * {@link RgbLed.setHsv}, {@link RgbLed.fillHsv}), then sent to the strip by
 * {@link RgbLed.show}.
 *
 * @example
 * import { RgbLed } from "stdlib/rgbled";
 *
 * const led = new RgbLed(48, { count: 1, type: "ws2812" });
 * led.set(0, 255, 0, 0);          // LED 0 red
 * await led.show();
 * for (let hue = 0; ; hue += 2) {  // a rainbow
 *     led.fillHsv(hue, 1, 0.1);
 *     await led.show();
 * }
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | — | ✓ | — | ✓ |
 *
 * Where `device/rmt` is: importing it fails elsewhere.
 *
 * @module stdlib/rgbled
 */
declare module "stdlib/rgbled" {
    import type { Rmt } from "device/rmt";

    /**
     * The models of LED known to {@link RgbLed}: `"ws2812"` (GRB order,
     * 0.4 / 0.8 us high times, 300 us reset; WS2812B, SK6812 RGB and
     * compatible).
     */
    export type RgbLedType = "ws2812";

    /**
     * Options of `new RgbLed(pin, options)`. Unknown fields are ignored.
     */
    export interface RgbLedOptions {
        /** Number of LEDs of the strip, an integer >= 1. Default: 1. */
        count?: number;

        /** Model of the LEDs. Default: `"ws2812"`. */
        type?: RgbLedType;
    }

    /**
     * How a model of LED is driven (the entry of the model's table).
     */
    export interface RgbLedModel {
        /** Order of the colors on the wire, one letter each (`"grb"`). */
        order: string;

        /** Ticks per second of the timings below (10,000,000: 0.1 us). */
        resolution: number;

        /** A 0 bit: [high, low] durations, in ticks. */
        bit0: [number, number];

        /** A 1 bit: [high, low] durations, in ticks. */
        bit1: [number, number];

        /** Time low after the bits that latches the colors, in ticks. */
        reset: number;
    }

    /**
     * A strip of addressable RGB LEDs on one pin.
     *
     * The colors are kept in memory (all off at first) and sent by
     * {@link RgbLed.show}; the LEDs keep showing the last colors sent, even
     * after {@link RgbLed.close} and after the end of the program. A
     * `show()` in progress keeps the program running until it is over.
     *
     * Color components are numbers, rounded and kept in 0..255; a value that
     * does not convert to a finite number is a `TypeError`.
     *
     * @example
     * import { RgbLed } from "stdlib/rgbled";
     *
     * const strip = new RgbLed(5, { count: 8 });
     * strip.fill(0, 0, 32);       // every LED dim blue
     * strip.set(0, 255, 255, 0);  // the first one yellow
     * await strip.show();
     * strip.close();
     */
    export class RgbLed {
        /**
         * Opens the strip: takes the pin and a channel of the RMT
         * peripheral. Nothing is sent before {@link RgbLed.show}.
         *
         * @param pin the GPIO of the data line, an integer from 0 to 63
         *     (not an integer: `TypeError`; out of range: `RangeError`).
         * @param options the length of the strip and its model; omitted or
         *     `undefined`: one WS2812 (`null` is a `TypeError`).
         * @throws `RangeError` for an unknown `type` or a `count` that is not
         *     an integer >= 1; `Error` "rmt: cannot open a channel on pin N"
         *     when no RMT channel is free or the pin is refused.
         */
        constructor(pin: number, options?: RgbLedOptions);

        /**
         * The number of LEDs, as given to the constructor. Do not change it.
         */
        readonly count: number;

        /**
         * The model of the LEDs (shared by every strip of that type: do not
         * change it).
         */
        readonly model: RgbLedModel;

        /**
         * The colors as they will be sent: 3 bytes per LED, in the order of
         * the model (`"grb"` for a WS2812). {@link RgbLed.set} writes it;
         * writing it directly works too.
         */
        readonly bytes: Uint8Array;

        /**
         * The RMT channel that sends the bits (owned by the strip: closed by
         * {@link RgbLed.close}).
         */
        readonly rmt: Rmt;

        /**
         * Sets the color of one LED (sent by {@link RgbLed.show}).
         *
         * @param index the LED, an integer from 0 to `count - 1`, else a
         *     `RangeError`.
         * @param r red, 0 to 255.
         * @param g green, 0 to 255.
         * @param b blue, 0 to 255.
         */
        set(index: number, r: number, g: number, b: number): void;

        /**
         * Sets every LED to the same color (sent by {@link RgbLed.show}).
         *
         * @param r red, 0 to 255.
         * @param g green, 0 to 255.
         * @param b blue, 0 to 255.
         */
        fill(r: number, g: number, b: number): void;

        /**
         * Sets the color of one LED as hue, saturation and value (sent by
         * {@link RgbLed.show}).
         *
         * @param index the LED, an integer from 0 to `count - 1`, else a
         *     `RangeError`.
         * @param hue in degrees, any value (taken modulo 360: 0 red, 120
         *     green, 240 blue).
         * @param saturation 0 (white) to 1 (pure color), kept in that range.
         * @param value brightness, 0 (off) to 1 (full), kept in that range.
         * @throws `TypeError` when hue, saturation or value is not a number.
         */
        setHsv(index: number, hue: number, saturation: number, value: number): void;

        /**
         * Sets every LED to the same color, as hue (degrees), saturation and
         * value (0 to 1) (sent by {@link RgbLed.show}). See
         * {@link RgbLed.setHsv}.
         */
        fillHsv(hue: number, saturation: number, value: number): void;

        /**
         * Sends the colors to the strip.
         *
         * @returns a promise resolved (with `undefined`) once the bits and
         *     the reset time have been sent: the LEDs then show the colors;
         *     rejected with `Error` "rmt: closed while sending" if
         *     {@link RgbLed.close} is called before.
         * @throws synchronously `Error` "rmt: a send is already in progress"
         *     when the previous `show()` is not over (await it first),
         *     "rmt: closed" after {@link RgbLed.close}, "rmt: the hardware
         *     refused the send".
         */
        show(): Promise<void>;

        /**
         * Frees the pin and its RMT channel; the LEDs keep their colors. A
         * `show()` in progress is rejected. The strip can no longer be used.
         */
        close(): void;
    }
}
