/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Fixture of tests/bytes.test.js: imports one font of the standard library
// (stdlib/fonts/inter16, a file imported as bytes by a library module). Its
// package must hold that font only. tests/run-tests.sh runs it from its
// sources (the tool's embedded copy of the standard library) and from its
// package: both print the same line.
import inter16 from "stdlib/fonts/inter16";

// the first table of an LVGL binary font: its size (4 bytes), its tag
// ("head"), then the header, the font size at byte 14
const FIRST_TAG_START = 4;
const FIRST_TAG_END = 8;
const FONT_SIZE_OFFSET = 14;

const view = new DataView(inter16.buffer, inter16.byteOffset, inter16.byteLength);
const tag = String.fromCharCode(...inter16.subarray(FIRST_TAG_START, FIRST_TAG_END));
const fontSize = view.getUint16(FONT_SIZE_OFFSET, true);

console.log(`inter16: ${inter16.length} bytes, first table '${tag}', font size ${fontSize}`);
