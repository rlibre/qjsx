/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Inter Regular, 24 px, for text: printable ASCII and Latin-1. The bytes of
// a font file in LVGL's binary format (4 bits per pixel, not compressed),
// made by tools/fonts/make-stdlib-fonts.sh. One module per font: a program
// gets in its package only the fonts it imports. Inter: SIL Open Font
// License 1.1 (LICENSE-Inter.txt, next to this file).
import inter24 from "./inter24.bin" with { type: "bytes" };

export default inter24;
