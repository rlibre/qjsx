/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Fixture of tests/image.test.js and tests/run-tests.sh: a PNG imported as an
// image. Run from its sources, 'qjsx run' converts it on the fly; run from
// its package, it is the image 'qjsx build' converted. Both must print the
// same (expected.txt).
import logo from "./logo.png" with { type: "image" };

// the format of the renderer, and a sum of all its bytes
let sum = 0;
for (const byte of logo) {
    sum = (sum + byte) % 65521;
}

const view = new DataView(logo.buffer, logo.byteOffset, logo.byteLength);
console.log(`${String.fromCharCode(...logo.subarray(0, 4))} ${view.getUint16(6, true)} x ${view.getUint16(8, true)}`);
console.log(`${logo.length} bytes, sum ${sum}`);
