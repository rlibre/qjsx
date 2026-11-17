/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Fixture of tests/bytes.test.js: imports the same file as src/main.js, from
// another folder, and says what an array of bytes holds.
import glyphs from "../fonts/glyphs.bin" with { type: "bytes" };

// Size of fonts/glyphs.bin; its byte i is (i * 7 + 3) & 0xff (0x00, "\r" and
// "\n" among them).
export const GLYPHS_SIZE = 300;

/**
 * The expected value of byte 'index' of fonts/glyphs.bin.
 */
function expectedByte(index) {
    return (index * 7 + 3) & 0xff;
}

/**
 * { ok, text }: whether 'bytes' is a Uint8Array of 'size' bytes holding the
 * expected values, and a line that says what it holds.
 */
export function describe(name, bytes, size) {
    // every byte, and their sum
    let sum = 0;
    let asExpected = true;

    for (let i = 0; i < bytes.length; i++) {
        sum += bytes[i];
        if (bytes[i] !== expectedByte(i)) {
            asExpected = false;
        }
    }

    // the kind of array and its size
    const isUint8Array = bytes instanceof Uint8Array;
    const ok = isUint8Array && bytes.length === size && bytes.byteOffset === 0 && asExpected;
    const text = `${name}: ${bytes.constructor.name} of ${bytes.length} bytes, sum ${sum}, ${ok ? "as expected" : "WRONG"}`;
    return { ok, text };
}

/**
 * True if 'bytes' is the array this module imported: one module per file,
 * whatever the importer.
 */
export function isTheImportOfDescribe(bytes) {
    return bytes === glyphs;
}
