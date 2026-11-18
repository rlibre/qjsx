/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * Color displays wired on SPI (a controller with a data / command pin), from
 * the standard library: written in JS on top of `device/spi`, `device/gpio`
 * (reset pin), `device/pwm` (backlight) and the native renderer `sys/draw`.
 *
 * The drawing calls draw NOTHING: each one adds a command to a list (a few
 * bytes; no pixels are kept, there is no copy of the screen in memory).
 * {@link Display.show} hands a copy of the list to the task of the SPI bus,
 * which renders the image a band of rows at a time while it sends them: the
 * program goes on meanwhile, with an empty list for the next image.
 *
 * What follows from it:
 * - an image is made of everything drawn since the last `show()`: the display
 *   does not remember the image before. The program draws again everything
 *   in the area it shows; what no command covers there takes the
 *   {@link Display.background} color;
 * - `show(x, y, width, height)` sends one area only: the commands are drawn
 *   again there, cut to it, and the rest of the screen keeps what it shows;
 * - one image goes out at a time: a `show()` called while an image is still
 *   going out resolves `false` at once and keeps the list;
 * - a command added to a full list is dropped and counted in
 *   {@link Display.dropped}.
 *
 * Coordinates are pixels, (0, 0) at the top left of the screen as the
 * program sees it (after the rotation), x to the right, y down. The list is
 * an `Int16Array`: coordinates, sizes and radii are integers from -32768 to
 * 32767; a fraction is cut toward zero, a larger value wraps around. What
 * lies outside the screen or the area shown is cut. Colors are numbers
 * `0xRRGGBB`; the display shows them with 5 bits of red, 6 of green and 5 of
 * blue (the low bits are dropped). The drawing calls check nothing (they run
 * on a microcontroller, often many times per image): wrong arguments draw
 * something wrong, they do not throw.
 *
 * Text uses font files in LVGL's binary format, made WITHOUT compression by
 * `lv_font_conv --format bin --no-compress`, imported with
 * `with { type: "bytes" }`: they are read in place (in flash on a device),
 * never copied. See {@link Display.font}.
 *
 * Images are PNG files imported with `with { type: "image" }`: `qjsx build`
 * converts them for the display (16-bit pixels, dithered, their transparency
 * kept), and they are read in place too. See {@link Display.image}.
 *
 * @example
 * import { Display } from "stdlib/display";
 * import inter16 from "./fonts/inter16.bin" with { type: "bytes" };
 *
 * // the display of the WT32-SC01, landscape (480 x 320)
 * const lcd = new Display({
 *     type: "st7796", port: 2,
 *     sclk: 14, mosi: 13, cs: 15, dc: 21, reset: 22, backlight: 23,
 *     rotation: 90,
 * });
 * const label = lcd.font(inter16);
 *
 * lcd.background = 0x102030;
 * lcd.fillRoundRect(20, 20, 200, 60, 12, 0x203050);
 * lcd.text(36, 40, "Température", label, 0xffffff);
 * lcd.arc(360, 160, 90, -135, 45, 14, 0x30c060);   // a gauge: 0 at the top, clockwise
 * await lcd.show();                                 // now the image is on the screen
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | i.MX RT1062 | Twin |
 * |---|---|---|---|---|
 * | — | ✓ ¹ | — | — | ✓ ² |
 *
 * ¹ Boards with a display, whose firmware has `sys/draw` (today the
 * WT32-SC01). Elsewhere importing it fails.
 *
 * ² The twin of such a board (`wt32-sc01`): the screen is drawn in the page.
 *
 * @module stdlib/display
 */
declare module "stdlib/display" {
    /**
     * The display controllers known to {@link Display}: `"st7796"` (320 x 480
     * pixels at rotation 0, SPI clock 40 MHz), and `"builtin"`: the display
     * built into the board (the M5Stack Tab5's, 720 x 1280 at rotation 0,
     * MIPI-DSI): no bus nor pins to give but its `backlight` (22 on the
     * Tab5); its `show()` draws into the image the panel shows, on a task
     * of its own (its promise is settled once the screen shows the area).
     */
    export type DisplayType = "st7796" | "builtin";

    /**
     * How the image is turned, clockwise, in degrees. At 90 and 270 the
     * width and height of the controller are exchanged.
     */
    export type DisplayRotation = 0 | 90 | 180 | 270;

    /**
     * Options of `new Display(options)`. The pins are GPIO numbers. Unknown
     * fields are ignored.
     */
    export interface DisplayOptions {
        /**
         * The controller. Default `"st7796"`; another one is a `RangeError`,
         * and so is `"builtin"` on a board without a built-in display.
         */
        type?: DisplayType;

        /**
         * The SPI port of the bus (2 or 3 on the ESP32). See `device/spi`.
         * Not for `"builtin"`.
         */
        port?: number;

        /**
         * Clock pin of the bus. Required by `device/spi` (`TypeError`). Not
         * for `"builtin"`.
         */
        sclk?: number;

        /** Data out pin of the bus (MOSI). */
        mosi?: number;

        /**
         * Chip select pin of the controller. Absent: none is driven (a
         * controller whose chip select is wired active).
         */
        cs?: number;

        /**
         * Data / command pin of the controller. Required on SPI
         * (`TypeError`); not for `"builtin"`.
         */
        dc?: number;

        /**
         * Reset pin of the controller: pulsed low when the display opens,
         * then kept high. Absent: no reset pulse.
         */
        reset?: number;

        /**
         * Backlight pin: a PWM output at 5 kHz, fully on when the display
         * opens (see {@link Display.backlight}). Absent: the program does not
         * drive the backlight.
         */
        backlight?: number;

        /**
         * SPI clock, Hz. Default: the controller's (40 MHz for the ST7796,
         * its limit: at 80 MHz it shows a white screen).
         */
        frequency?: number;

        /** How the image is turned, clockwise. Default 0; another value is a `RangeError`. */
        rotation?: DisplayRotation;

        /**
         * Room of the list, in commands of 6 items: an arc and a rounded
         * rectangle take 8 items, a text 6 plus one per UTF-16 unit of its
         * string, `rect()` 4 commands. Default 256 (an `Int16Array` of 3 KB in
         * the JS heap).
         */
        commands?: number;

        /**
         * Rows rendered and sent at a time, an integer >= 1. Default 16; at
         * most what one transfer of the bus takes (32 KB: 34 rows of 480
         * pixels). While an image goes out, two buffers of that many rows are
         * taken from the RAM of the firmware (16 rows of 480 pixels: 2 x 15 KB).
         */
        bandRows?: number;
    }

    /**
     * Options of {@link Display.font}.
     */
    export interface DisplayFontOptions {
        /**
         * The digits 0 to 9 all take the width of the widest one, each one
         * centered in it: a value that changes does not move what follows it.
         * Default false (the widths of the font).
         */
        tabularDigits?: boolean;
    }

    /**
     * A font made by {@link Display.font}, for {@link Display.text} and
     * {@link Display.textWidth} of THE SAME display (its index is the
     * display's). Do not change its fields. Metrics are in pixels.
     */
    export interface DisplayFont {
        /** Its place among the font files of the display, 0 to 15. */
        readonly index: number;

        /** Its options as the renderer takes them (1: tabular digits). */
        readonly flags: number;

        /** The font file, as given (kept by the display while it lives). */
        readonly bytes: Uint8Array;

        /** The size of the font, as given to the converter. */
        readonly size: number;

        /** Pixels from the top of a line to its baseline. */
        readonly ascent: number;

        /** Pixels from the baseline to the bottom of a line, 0 or NEGATIVE. */
        readonly descent: number;

        /** The height of a line: `ascent - descent`. */
        readonly lineHeight: number;
    }

    /**
     * An image made by {@link Display.image}, for {@link Display.drawImage}
     * of THE SAME display (its index is the display's). Do not change its
     * fields.
     */
    export interface DisplayImage {
        /** Its place among the images of the display, 0 to 15. */
        readonly index: number;

        /** Its width, pixels. */
        readonly width: number;

        /** Its height, pixels. */
        readonly height: number;

        /** The image file, as given (kept by the display while it lives). */
        readonly bytes: Uint8Array;
    }

    /**
     * A color display on an SPI bus.
     *
     * Drawing calls add commands to the image being made; {@link Display.show}
     * sends it. The display owns its bus, its reset pin and its backlight
     * until {@link Display.close}.
     *
     * @example
     * import { Display } from "stdlib/display";
     * import inter40 from "./fonts/inter40.bin" with { type: "bytes" };
     *
     * const lcd = new Display({ port: 2, sclk: 14, mosi: 13, cs: 15, dc: 21, reset: 22, backlight: 23 });
     * const big = lcd.font(inter40, { tabularDigits: true });
     *
     * // a value centered on a band of the screen, sent alone: the rest of the
     * // screen stays, the rest of the band takes the background
     * function showValue(value) {
     *     const text = `${value.toFixed(1)} °C`;
     *     lcd.text((lcd.width - lcd.textWidth(text, big)) >> 1, 200, text, big, 0xffff00);
     *     return lcd.show(0, 200, lcd.width, big.lineHeight);
     * }
     * await showValue(21.5);
     */
    export class Display {
        /**
         * Opens the display: takes the SPI bus, pulses the reset pin, sets
         * the controller up (16 bits per pixel, the rotation), turns the
         * screen and the backlight on. It BLOCKS the program about a quarter
         * of a second (the controller's waits). The screen shows what its
         * memory holds until the first {@link Display.show}.
         *
         * @throws `RangeError` for an unknown `type` or a `rotation` that is
         *     not 0, 90, 180 or 270; `TypeError` without `dc`; the errors of
         *     `device/spi` (bus and device), `device/gpio` (reset) and
         *     `device/pwm` (backlight) when a port or a pin cannot be used:
         *     an `Error` with a `code` (`"ENOENT"`, `"EBUSY"`, `"EINVAL"`), or
         *     a `TypeError` / `RangeError` for a wrong pin or frequency.
         */
        constructor(options: DisplayOptions);

        /** The controller, as given. */
        readonly type: DisplayType;

        /** The rotation, as given. */
        readonly rotation: DisplayRotation;

        /** Width of the screen as the program sees it (after the rotation), pixels. */
        readonly width: number;

        /** Height of the screen as the program sees it (after the rotation), pixels. */
        readonly height: number;

        /**
         * Color (`0xRRGGBB`) of what no command covers in the area shown,
         * read by {@link Display.show}. Default `0x000000` (black).
         */
        background: number;

        /**
         * Commands dropped because the list was full, since the last
         * {@link Display.clear} or {@link Display.show} (which empties it:
         * read it BEFORE showing). Not 0: the image lacks them; give the
         * display more `commands`.
         */
        dropped: number;

        /**
         * Adds a filled rectangle, its top left pixel at (x, y). A width or
         * height of 0 or less draws nothing.
         */
        fillRect(x: number, y: number, width: number, height: number, color: number): void;

        /**
         * Adds the outline of a rectangle, one pixel thick, inside its box
         * (4 commands).
         */
        rect(x: number, y: number, width: number, height: number, color: number): void;

        /**
         * Adds a line from (x0, y0) to (x1, y1), both ends included, one
         * pixel thick (not smoothed).
         */
        line(x0: number, y0: number, x1: number, y1: number, color: number): void;

        /** Adds one pixel. */
        pixel(x: number, y: number, color: number): void;

        /**
         * Adds an arc of a ring centered on (cx, cy): from `radius` (its
         * outer edge) inward over `thickness` pixels (less than 1: nothing;
         * `radius` or more: no hole, so a filled sector, a slice of pie: the
         * way to draw one, there is no other), from angle `start`
         * to `end`, in degrees, 0 at the top, clockwise (90 at the right, -90
         * at the left). The angles are kept to a tenth of a degree. An `end`
         * at or before `start` draws nothing; 360 degrees or more draw the
         * whole ring. Its edges are smoothed (blended over what is below);
         * its ends are cut along the radius.
         *
         * @example
         * // a gauge of 270 degrees, its value from 0 to 1
         * lcd.arc(120, 120, 100, -135, 135, 16, 0x303030);
         * lcd.arc(120, 120, 100, -135, -135 + 270 * value, 16, 0x30c060);
         */
        arc(cx: number, cy: number, radius: number, start: number, end: number, thickness: number, color: number): void;

        /**
         * Adds a circle: a whole ring of `thickness` pixels inside `radius`,
         * centered on (cx, cy), smoothed.
         */
        circle(cx: number, cy: number, radius: number, thickness: number, color: number): void;

        /** Adds a filled disc of `radius` centered on (cx, cy), its edge smoothed. */
        disc(cx: number, cy: number, radius: number, color: number): void;

        /**
         * Adds a filled rectangle with rounded corners, its top left pixel at
         * (x, y): the same `radius` at the four corners, at most half the
         * smaller side (a larger one is reduced to it). The corners are
         * smoothed.
         */
        fillRoundRect(x: number, y: number, width: number, height: number, radius: number, color: number): void;

        /**
         * Adds the outline of a rectangle with rounded corners: `thickness`
         * pixels wide, inside its box (0, or at least half the smaller side:
         * filled, as {@link Display.fillRoundRect}). The corners are smoothed.
         */
        roundRect(x: number, y: number, width: number, height: number, radius: number, thickness: number, color: number): void;

        /**
         * Makes a font for {@link Display.text} from a font file: LVGL's
         * binary format WITHOUT compression (`lv_font_conv --format bin
         * --no-compress`; 1 to 8 bits per pixel; its kerning is ignored),
         * imported with `with { type: "bytes" }`. The file is read in place
         * and kept by the display while it lives. The same file may be given
         * again (with other options): it still counts once.
         *
         * @throws `TypeError` when `bytes` is not a `Uint8Array` or not such
         *     a font (a compressed one included); `RangeError` for a 17th
         *     font file on the same display (at most 16).
         */
        font(bytes: Uint8Array, options?: DisplayFontOptions): DisplayFont;

        /**
         * Adds ONE line of text in `font` (made by {@link Display.font} of
         * this display), the top left of its line at (x, y): its baseline is
         * `font.ascent` pixels below y. Its glyphs are blended over what is
         * below them. A character the font lacks is skipped (a line break
         * included: there is no second line). Each UTF-16 unit of `string`
         * takes one item of the list; when they do not fit, the whole text
         * is dropped.
         */
        text(x: number, y: number, string: string, font: DisplayFont, color: number): void;

        /**
         * The width in pixels (rounded up) of `string` drawn by
         * {@link Display.text} in `font`, its options included: to center or
         * align a text on the right.
         *
         * @throws `TypeError` when `string` is not a string.
         */
        textWidth(string: string, font: DisplayFont): number;

        /**
         * Makes an image for {@link Display.drawImage} from a PNG file
         * imported with `with { type: "image" }`: `qjsx build` converted it
         * (16-bit pixels, dithered, transparency kept; `qjsx run script.js`
         * converts it when the program starts). The file is read in place
         * and kept by the display while it lives. The same file given again
         * counts once.
         *
         * @example
         * import logoFile from "./logo.png" with { type: "image" };
         * const logo = lcd.image(logoFile);
         * lcd.drawImage(lcd.width - logo.width - 8, 8, logo);
         *
         * @throws `TypeError` when `bytes` is not a `Uint8Array` or not such
         *     an image (a PNG imported with `type: "bytes"` included);
         *     `RangeError` for a 17th image on the same display (at most 16).
         */
        image(bytes: Uint8Array): DisplayImage;

        /**
         * Adds an image made by {@link Display.image} of this display, its
         * top left pixel at (x, y). Its transparent pixels leave what is
         * below them, its partly transparent ones (smoothed edges) are
         * blended over it. What lies outside the screen is cut.
         */
        drawImage(x: number, y: number, image: DisplayImage): void;

        /**
         * Empties the image being made without showing it, and sets
         * {@link Display.dropped} back to 0.
         */
        clear(): void;

        /**
         * Shows the image (everything drawn since the last `show()`) on the
         * whole screen, or on the area given (cut to the screen), and empties
         * the list for the next image. The area costs its pixels: sending
         * them takes about 61 ms for the whole 320 x 480 screen at 40 MHz, a
         * quarter of it for a quarter of the screen. The image goes out in
         * the background; the list may be filled again at once.
         *
         * @returns a promise resolved with `true` once the image is on the
         *     screen; `true` at once when the area lies outside the screen
         *     (the image is dropped); `false` at once, the list KEPT, when
         *     another image is still going out (await it first). Rejected
         *     with the `Error` of `device/spi` when the transfer fails.
         * @throws synchronously an `Error` with `code` `"EBADF"` after
         *     {@link Display.close}.
         */
        show(x?: number, y?: number, width?: number, height?: number): Promise<boolean>;

        /**
         * Sets the brightness of the backlight, 0 (off) to 1 (full).
         *
         * @throws `Error` when no `backlight` pin was given; `RangeError`
         *     outside 0 to 1; `Error` after {@link Display.close}.
         */
        backlight(level: number): void;

        /**
         * Frees the bus, the reset pin and the backlight pin (the backlight
         * goes off). An image going out is sent to its end first. The screen
         * keeps its image; the display can no longer be used.
         */
        close(): void;
    }
}
