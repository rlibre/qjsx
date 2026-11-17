/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Inter Regular, 40 px, for values: printable ASCII and the degree sign only
// (with Latin-1 the file would be twice as large). The bytes of a font file
// in LVGL's binary format (4 bits per pixel, not compressed), made by
// tools/fonts/make-stdlib-fonts.sh. One module per font: a program gets in
// its package only the fonts it imports. Inter: SIL Open Font License 1.1
// (LICENSE-Inter.txt, next to this file).
import inter40 from "./inter40.bin" with { type: "bytes" };

export default inter40;
