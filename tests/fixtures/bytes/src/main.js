/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Fixture of tests/bytes.test.js: imports files as bytes. tests/run-tests.sh
// runs it from its sources and from its package: both print the same lines,
// and the program ends with code 1 when the bytes are not the expected ones.
// src/unused.bin is never imported: the package does not hold it.
import glyphs from "./fonts/glyphs.bin" with { type: "bytes" };
import empty from "./empty.bin" with { type: "bytes" };
import { describe, isTheImportOfDescribe, GLYPHS_SIZE } from "./lib/describe.js";

const glyphsLine = describe("fonts/glyphs.bin", glyphs, GLYPHS_SIZE);
const emptyLine = describe("empty.bin", empty, 0);
const sameModule = isTheImportOfDescribe(glyphs);

console.log(glyphsLine.text);
console.log(emptyLine.text);
console.log(`one module for every importer: ${sameModule}`);

// wrong bytes: the test fails
const allRight = glyphsLine.ok && emptyLine.ok && sameModule;
if (!allRight) {
    process.exit(1);
}
