/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The type of a file imported as bytes, so that the editor knows the import
// of a .bin file (a font of stdlib/display, a table...). Not a module of the
// runtime: tests/types.test.js leaves wildcard declarations out.

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
