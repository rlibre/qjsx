/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * Inter Regular at 40 pixels, a font for values, from the standard library:
 * printable ASCII (`0x20`-`0x7E`) and the degree sign `°` only, no accented
 * letter (with Latin-1 the file would be twice as large).
 *
 * The default export is the font file itself, 24,192 bytes in LVGL's binary
 * font format (4 bits per pixel, not compressed, no kerning), meant for the
 * large values of a display. The file is imported as bytes: on a device the
 * array lies where the package is (flash), nothing is copied into RAM, and
 * only a program that imports this module has the file in its package (one
 * module per font for that reason: `stdlib/fonts/inter16`,
 * `stdlib/fonts/inter24`).
 *
 * The array is read-only: writing into it is an error that is not detected.
 *
 * Inter is by The Inter Project Authors, under the SIL Open Font License 1.1
 * (`lib/stdlib/src/fonts/LICENSE-Inter.txt`).
 *
 * @example
 * import inter40 from "stdlib/fonts/inter40";
 *
 * console.log(inter40.length);   // 24192
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | i.MX RT1062 | Twin |
 * |---|---|---|---|---|
 * | ✓ | ✓ | ✓ | ✓ | ✓ |
 *
 * @module stdlib/fonts/inter40
 */
declare module "stdlib/fonts/inter40" {
    /**
     * The font file, read-only.
     */
    const inter40: Uint8Array;
    export default inter40;
}
