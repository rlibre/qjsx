/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * The touch controllers of displays, on I2C, from the standard library:
 * written in JS on top of `device/i2c` and `device/gpio` (the interrupt pin).
 *
 * The points are given in the coordinates of the drawing of a
 * `stdlib/display` (its size and rotation), so that a finger lands where the
 * program drew.
 *
 * How it reads:
 * - with an interrupt pin, nothing is read while nobody touches: the
 *   controller's signal starts the reads. Without one, the controller is read
 *   `rate` times a second, always;
 * - while a finger is on the screen, the controller is read `rate` times a
 *   second at most: the reports it makes in between (about 100 a second) are
 *   not read, the next read takes the position of the moment;
 * - {@link Touch.onChange} is called when a finger comes or goes, or moves by
 *   `threshold` pixels or more;
 * - {@link Touch.points} is made once, when the touch is opened: its objects
 *   are updated in place, nothing is made at each move. A program that keeps
 *   a point copies its `x` and `y`.
 *
 * @example
 * import { Display } from "stdlib/display";
 * import { Touch } from "stdlib/touch";
 *
 * // the WT32-SC01, landscape
 * const lcd = new Display({
 *     type: "st7796", port: 2,
 *     sclk: 14, mosi: 13, cs: 15, dc: 21, reset: 22, backlight: 23,
 *     rotation: 90,
 * });
 * const touch = new Touch({ type: "ft6336", port: 0, sda: 18, scl: 19, interrupt: 39, display: lcd });
 *
 * touch.onChange(() => {
 *     for (let i = 0; i < touch.count; i++) {
 *         const point = touch.points[i];
 *         lcd.disc(point.x, point.y, 6, 0xff4040);
 *         lcd.show(point.x - 6, point.y - 6, 13, 13);
 *     }
 * });
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | i.MX RT1062 | Twin |
 * |---|---|---|---|---|
 * | — | ✓ ¹ | — | — | ✓ ² |
 *
 * ¹ Boards with a touch controller on I2C: the WT32-SC01 (FT6336) and the
 * M5Stack Tab5 (GT911, started by the firmware of the board).
 *
 * ² The twins of the WT32-SC01 (FT6336) and of the M5Stack Tab5 (GT911):
 * touched with the mouse on the screen of the page (one finger); a project
 * may add such a part to another board.
 *
 * @module stdlib/touch
 */
declare module "stdlib/touch" {
    import type { I2c } from "device/i2c";

    /**
     * The touch controllers known to {@link Touch}:
     * - `"gt911"` (Goodix): up to 10 points (what its configuration sets),
     *   address 0x14; the range of its coordinates is read from its
     *   configuration;
     * - `"ft6336"` (FocalTech): up to 2 points, address 0x38; its coordinates
     *   are pixels of the panel.
     */
    export type TouchType = "gt911" | "ft6336";

    /**
     * Options of `new Touch(options)`. The pins are GPIO numbers. Unknown
     * fields are ignored.
     */
    export interface TouchOptions {
        /** The controller (another one is a `RangeError`). */
        type: TouchType;

        /**
         * An I2C bus the program opened (shared with other devices); it stays
         * open after {@link Touch.close}. Without it, the touch opens the bus
         * `port` on `sda` and `scl` itself, at 400 kHz, and closes it.
         */
        bus?: I2c;

        /** The I2C port of the bus to open (see `device/i2c`). */
        port?: number;

        /** Data pin of the bus to open. */
        sda?: number;

        /** Clock pin of the bus to open. */
        scl?: number;

        /** The I2C address of the controller. Default: the type's. */
        address?: number;

        /**
         * The pin of the controller's interrupt signal. Without it, the
         * controller is read `rate` times a second, always.
         */
        interrupt?: number;

        /**
         * Reads a second at most, above 0, 1000 at most (`RangeError`).
         * Default 33 (one read every 30 ms).
         */
        rate?: number;

        /**
         * The smallest move reported, in pixels. Default 2 (a finger held
         * still trembles by a pixel or two).
         */
        threshold?: number;

        /**
         * The display the coordinates are given for: its `width`, `height` and
         * `rotation`. Or give these three instead.
         */
        display?: { readonly width: number; readonly height: number; readonly rotation: number };

        /** The width of the screen as the program sees it (without `display`). */
        width?: number;

        /** Its height (without `display`). */
        height?: number;

        /**
         * How the screen is turned, clockwise, in degrees: 0, 90, 180 or 270
         * (`RangeError` otherwise). Default 0 (without `display`).
         */
        rotation?: number;

        /**
         * How the touch panel is mounted on the display, when the
         * controller's coordinates are not those of the display at rotation
         * 0 (a board setting: the same controller may be mounted otherwise
         * on another module). The controller's x and y exchanged, applied
         * first. Default false (the WT32-SC01 and the M5Stack Tab5).
         */
        swapXY?: boolean;

        /** The controller's x mirrored (after `swapXY`). Default false. */
        mirrorX?: boolean;

        /** The controller's y mirrored (after `swapXY`). Default false. */
        mirrorY?: boolean;
    }

    /**
     * A finger on the screen ({@link Touch.points}).
     */
    export interface TouchPoint {
        /**
         * The controller's number of this finger: the same while it stays on
         * the screen.
         */
        id: number;

        /** Its position, in the coordinates of the drawing. */
        x: number;

        /** Its position, in the coordinates of the drawing. */
        y: number;
    }

    /**
     * The touch controller of a display.
     */
    export class Touch {
        /**
         * Opens the controller and reads its configuration, then starts
         * reading.
         *
         * @throws a `RangeError` for an unknown type, rotation or rate, a
         *     `TypeError` without the size of the screen; the errors of
         *     `device/i2c` (`"ENXIO"`: no controller at the address) and of
         *     `device/gpio`. What was opened is closed again.
         */
        constructor(options: TouchOptions);

        /**
         * The fingers: {@link maxPoints} objects made once, updated in place;
         * the first {@link count} are the fingers on the screen.
         */
        readonly points: readonly TouchPoint[];

        /** How many fingers are on the screen. */
        readonly count: number;

        /** The most fingers the controller reports (its configuration). */
        readonly maxPoints: number;

        /** The width of the screen as the program sees it, pixels. */
        readonly width: number;

        /** Its height, pixels. */
        readonly height: number;

        /** How the screen is turned, clockwise, in degrees. */
        readonly rotation: number;

        /**
         * Calls `callback()` when a finger comes or goes, or moves by
         * `threshold` pixels or more; {@link points} and {@link count} are
         * up to date then. `null`: no more calls. One callback at a time.
         *
         * @throws `TypeError` when `callback` is neither a function nor null.
         */
        onChange(callback: (() => void) | null): void;

        /**
         * Stops reading; frees the interrupt pin, the controller, and the
         * bus if the touch opened it. Closing twice does nothing.
         */
        close(): void;
    }
}
