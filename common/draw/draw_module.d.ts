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
 *
 * The commands are drawn in their order, the later ones over the earlier
 * ones, over the background. Another kind draws nothing (6 items skipped);
 * a command cut by the end of the items, and a text whose font is not
 * given, are skipped. Nothing is checked beyond that.
 *
 * Fonts are files in LVGL's binary bitmap format made WITHOUT compression
 * (`lv_font_conv --format bin --no-compress`; 1, 2, 3, 4 or 8 bits per
 * pixel; the kerning is ignored), as `Uint8Array`s: read in place (in flash
 * for a file imported with `with { type: "bytes" }` from the package), never
 * copied.
 *
 * Every number argument is a strict integer (a fraction, NaN or a string is
 * a `TypeError`, a value beyond 32 bits a `RangeError`).
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | i.MX RT1062 | Twin |
 * |---|---|---|---|---|
 * | — | ✓ ¹ | — | — | ✓ ² |
 *
 * ¹ Only in a firmware built for a board with a display (`QJSX_DRAW`, today
 * the WT32-SC01): a board without a screen pays nothing for it.
 *
 * ² The twin of a board that lists it (`wt32-sc01`).
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
    }

    /**
     * Renders the first `count` items of `items` into the area
     * (x, y, width, height) of the drawing and sends its pixels, row after
     * row, as one write on `device`. The controller must already expect them
     * (its column and row ranges and its memory write command are the
     * caller's). The items are copied at once: the caller may write the next
     * image right away. The font files are NOT copied: the caller keeps them
     * alive until the promise settles.
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
     * @returns a promise resolved once the last pixel is sent; rejected with
     *     an `Error` with a `code` from `device/spi` (`"EBADF"` closed,
     *     `"EAGAIN"` too many transactions pending on the bus, `"EIO"`,
     *     `"ECANCELED"`).
     * @throws synchronously a `TypeError` (`device` is not an `SpiDevice`,
     *     `items` not a typed array, a font that is not one) or a
     *     `RangeError` (an area or `bandRows` below 1, more than 16 fonts).
     */
    export function show(device: SpiDevice, items: Int16Array, count: number, x: number, y: number, width: number, height: number, background: number, bandRows: number, fonts?: readonly Uint8Array[]): Promise<void>;

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
