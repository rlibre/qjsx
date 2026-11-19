/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * The renderer of the displays: the native engine of `stdlib/display`, NOT
 * meant to be used directly (import `stdlib/display`, which writes the
 * commands, keeps the fonts and talks to the controller).
 *
 * It shows a list of drawing commands on a display wired on SPI. The list is
 * an `Int16Array` the JS code writes itself (no native call per command: a
 * call from JS to native code costs about 0.25 ms on an ESP32, writing a
 * command a few microseconds). {@link show} copies it and queues ONE write of
 * the area's pixels on the display's `SpiDevice`: the task of the SPI bus
 * renders each band of rows into a small buffer just before it goes out (the
 * next band is made while the previous one is sent), so the JS thread is
 * free meanwhile and no memory holds the whole image.
 *
 * A command is 6 items, `[kind, pixel, x0, y0, x1, y1]`, some followed by
 * more items. `pixel` is its color as the 16 bits the controller takes (5 of
 * red, 6 of green, 5 of blue, red in the high bits); coordinates are pixels
 * in the coordinates of the drawing:
 *
 * | kind | draws | x0, y0 | x1, y1 | items after it |
 * |---|---|---|---|---|
 * | 0 | a filled rectangle | its first pixel | width, height | none |
 * | 1 | a line, one pixel thick, both ends included | its start | its end | none |
 * | 2 | a line of text | the top left of its line | x1: font index (0..15, the low 4 bits) \| flags << 4 (1: tabular digits); y1: its length in UTF-16 units | its UTF-16 units, one item each |
 * | 3 | an arc of a ring, smoothed | its center | outer radius, thickness inward | start and end angles in tenths of a degree, 0 at the top, clockwise |
 * | 4 | a rectangle with rounded corners, smoothed | its first pixel | width, height | the radius of its corners, its thickness (0: filled) |
 * | 5 | an image (`pixel` and y1 unused) | its top left pixel | x1: image index (0..15) | none |
 *
 * The commands are drawn in their order, the later ones over the earlier
 * ones, over the background. Another kind draws nothing (6 items skipped);
 * a command cut by the end of the items, a text whose font is not given and
 * an image that is not given, are skipped. Nothing is checked beyond that.
 *
 * Fonts are files in LVGL's binary bitmap format made WITHOUT compression
 * (`lv_font_conv --format bin --no-compress`; 1, 2, 3, 4 or 8 bits per
 * pixel; the kerning is ignored), as `Uint8Array`s: read in place (in flash
 * for a file imported with `with { type: "bytes" }` from the package), never
 * copied. Images are PNG files imported with `with { type: "image" }`, which
 * the tool converted into the format of this renderer
 * (common/draw/image.h), read in place the same way.
 *
 * Every number argument is a strict integer (a fraction, NaN or a string is
 * a `TypeError`, a value beyond 32 bits a `RangeError`).
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | i.MX RT1062 | Twin |
 * |---|---|---|---|---|
 * | — | ✓ ¹ | — | — | ✓ ² |
 *
 * ¹ Only in a firmware built for a board with a display (`QJSX_DRAW`): the
 * WT32-SC01, whose ST7796 is driven over SPI, and the two ESP32-P4 boards
 * (M5Stack Tab5, Waveshare ESP32-P4-Module-DEV-KIT), whose panel is built
 * into the board and drawn by a task. A board without a screen pays nothing
 * for it.
 *
 * ² The twin of a board that lists it (`wt32-sc01`; `m5stack-tab5`, whose
 * display built in is an image in memory drawn in the page).
 *
 * Also on a Linux board run by `qjsx agent --screen` or `--touch` (the
 * STM32MP157-DK2): its display through DRM, its touch panel as the kernel
 * reads it.
 *
 * @module sys/draw
 */
declare module "sys/draw" {
    import type { SpiDevice } from "device/spi";

    /**
     * The metrics of a font file, in pixels ({@link fontMetrics}).
     */
    export interface DrawFontMetrics {
        /** The size of the font, as given to the converter. */
        size: number;

        /** From the top of a line to its baseline. */
        ascent: number;

        /** From the baseline to the bottom of a line, 0 or NEGATIVE. */
        descent: number;

        /** The height of a line: `ascent - descent`. */
        lineHeight: number;

        /** The height of the capital letters above the baseline (of `H`). */
        capHeight: number;
    }

    /**
     * Renders the first `count` items of `items` into the area
     * (x, y, width, height) of the drawing and sends it as ONE transaction on
     * `device`: the area told to the controller (MIPI DCS commands 0x2A
     * column range, 0x2B row range, 0x2C memory write, with DC low for the
     * commands), then the pixels, row after row. The coordinates are those of
     * the controller's memory as its rotation turns it (`stdlib/display` sets
     * it so). The items are copied at once: the caller may write the next
     * image right away. The font and image files are NOT copied: the caller
     * keeps them alive until the promise settles.
     *
     * Two buffers of `bandRows` rows of the area (2 bytes per pixel) and the
     * copy of the items are taken from the RAM of the firmware until the end.
     *
     * @param device the `SpiDevice` of the display, with its DC pin.
     * @param items the commands (see the table of the module).
     * @param count how many items to draw (more than `items` holds: all of
     *     them; below 0: none).
     * @param x left of the area, in the coordinates of the commands.
     * @param y top of the area.
     * @param width width of the area, pixels, >= 1.
     * @param height height of the area, pixels, >= 1.
     * @param background the 16-bit pixel of what no command covers.
     * @param bandRows rows rendered and sent at a time, >= 1 (at most the
     *     area's height is used).
     * @param fonts the font files the texts use, by index (at most 16);
     *     not an array: no fonts.
     * @param images the image files the image commands draw, by index (at
     *     most 16); not an array: no images.
     * @returns a promise resolved once the last pixel is sent; rejected with
     *     an `Error` with a `code` from `device/spi` (`"EBADF"` closed,
     *     `"EAGAIN"` too many transactions pending on the bus, `"EIO"`,
     *     `"ECANCELED"`).
     * @throws synchronously a `TypeError` (`device` is not an `SpiDevice`,
     *     `items` not a typed array, a font or an image that is not one) or
     *     a `RangeError` (an area or `bandRows` below 1, more than 16 fonts
     *     or images).
     */
    export function show(device: SpiDevice, items: Int16Array, count: number, x: number, y: number, width: number, height: number, background: number, bandRows: number, fonts?: readonly Uint8Array[], images?: readonly Uint8Array[]): Promise<void>;

    /**
     * The size of an image of this renderer, in pixels.
     */
    export interface DrawImageSize {
        /** Its width. */
        width: number;

        /** Its height. */
        height: number;
    }

    /**
     * Reads the header of an image file (a PNG imported with `with { type:
     * "image" }`).
     *
     * @throws `TypeError` when `image` is not a `Uint8Array` or not an image
     *     of this renderer.
     */
    export function imageSize(image: Uint8Array): DrawImageSize;

    /**
     * The size of the display built into the board (the M5Stack Tab5's),
     * as its panel scans it (rotation 0), in pixels; null on a board
     * without one.
     */
    export function screenSize(): DrawImageSize | null;

    /**
     * The display built into the board (the M5Stack Tab5's): its panel
     * shows an image held in memory, which {@link Screen.show} draws into.
     */
    export class Screen {
        /**
         * Takes the display built into the board.
         *
         * @throws an `Error` with code `"ENODEV"` on a board without one.
         */
        constructor();

        /**
         * Draws the area (x, y, width, height) of the drawing, turned by
         * `rotation` (0, 90, 180 or 270 degrees clockwise), into the image
         * of the display, a band of `bandRows` rows at a time, on a task of
         * its own (the JS thread goes on). The area must lie on the screen
         * as the drawing sees it. The other arguments are those of
         * {@link show}; the items are copied at once, the font and image
         * files are read until the promise settles.
         *
         * @returns a promise resolved once the screen shows the area.
         * @throws an `Error` with code `"EBADF"` (closed) or `"EBUSY"` (a
         *     frame is still being drawn); a `RangeError` for another
         *     rotation; the errors of {@link show} for the list, the fonts
         *     and the images.
         */
        show(items: Int16Array, count: number, x: number, y: number, width: number, height: number, background: number, bandRows: number, fonts: readonly Uint8Array[] | undefined, images: readonly Uint8Array[] | undefined, rotation: number): Promise<void>;

        /**
         * Lets the display go (a frame being drawn is finished first, its
         * promise rejected). The screen keeps its image.
         */
        close(): void;
    }

    /**
     * The touch panel the board's system reads itself (a Linux board run by
     * `qjsx agent --touch`: its kernel's driver), for `stdlib/touch` type
     * `"builtin"`: a callback at each new report, the report read into an
     * array (nothing made at each move). One at a time.
     */
    export class TouchPanel {
        /**
         * Takes the touch panel.
         *
         * @throws an `Error` with code `"ENODEV"` when the board reads none.
         */
        constructor();

        /** The most fingers it reports. */
        maxPoints: number;

        /** Its x coordinates: 0 to `rangeX - 1`, the panel at rotation 0. */
        rangeX: number;

        /** Its y coordinates: 0 to `rangeY - 1`. */
        rangeY: number;

        /**
         * Calls `callback()` at each new report (then {@link read} it); the
         * program is kept running while a callback is set. `null`: no more
         * calls.
         *
         * @throws `TypeError` when `callback` is neither a function nor null.
         */
        onChange(callback: (() => void) | null): void;

        /**
         * The last report into `points`: id, x, y for each finger, as many as
         * it holds.
         *
         * @returns how many fingers, or -1 when no report came since the last
         *     read.
         * @throws `TypeError` when `points` is not an `Int32Array`.
         */
        read(points: Int32Array): number;

        /** No more calls; the program no longer kept running by it. */
        close(): void;
    }

    /**
     * Reads the header of a font file.
     *
     * @throws `TypeError` when `font` is not a `Uint8Array` or not a font
     *     this renderer reads (a compressed one included).
     */
    export function fontMetrics(font: Uint8Array): DrawFontMetrics;

    /**
     * The width of `text` drawn in `font` with `flags` (1: tabular digits),
     * in whole pixels (rounded up), as a text command draws it. A character
     * the font lacks counts for nothing.
     *
     * @throws `TypeError` when `font` is not a font file or `text` not a
     *     string.
     */
    export function textWidth(font: Uint8Array, text: string, flags: number): number;
}
