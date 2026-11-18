/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The type of a file imported as bytes, so that the editor knows the import
// of a .bin file (a font of stdlib/display, a table...), and of a PNG file
// imported as an image. Not modules of the runtime: tests/types.test.js
// leaves wildcard declarations out.

/**
 * A file of the project imported as bytes:
 *
 * ```js
 * import inter16 from "./fonts/inter16.bin" with { type: "bytes" };
 * ```
 *
 * The bytes stay in the package: on a device they are read in place, in
 * flash, and nothing is copied to RAM. Only an imported file goes into the
 * package. Read only: never write into the array (on a device its memory is
 * the flash; the runtime does not stop the write).
 *
 * Any file may be imported this way; this declaration types the `.bin`
 * ones. For another extension, declare it the same way in a `.d.ts` of the
 * project.
 */
declare module "*.bin" {
    /** The bytes of the file. */
    const bytes: Uint8Array;
    export default bytes;
}

/**
 * A PNG file of the project imported as an image, for `stdlib/display`:
 *
 * ```js
 * import logo from "./logo.png" with { type: "image" };
 * ```
 *
 * `qjsx build` converts it into the format of the renderer of the displays
 * (16-bit pixels, dithered, its transparency kept), and that is what the
 * package holds and what the array gives: read in place, in flash on a
 * device. Give it to `Display.image()`. Read only, as the bytes of a file.
 * Interlaced PNG files are refused.
 */
declare module "*.png" {
    /** The image, converted for the displays. */
    const image: Uint8Array;
    export default image;
}
