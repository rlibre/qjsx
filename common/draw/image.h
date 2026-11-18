/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Images of the renderer (draw.h; user, 2026-11-18): a PNG file of a program
// is converted on the PC (host/js/image.js, imported with { type: "image" })
// into this format, which the renderer reads IN PLACE: on a device the file
// lies in the package, in flash, and nothing of it is copied.
//
// The format, our own (version 1), little endian:
//
//   offset 0   "QIMG"
//          4   version (1), 1 byte
//          5   0, 1 byte
//          6   width, pixels, 2 bytes
//          8   height, pixels, 2 bytes
//         10   0, 2 bytes
//         12   the offset of each row from the start of the file, 4 bytes
//              each: a band of the display goes straight to its rows
//         ...  the rows: runs, which cover exactly the width of the image
//
// A run starts with a byte: its kind in the 2 high bits, its length less
// one in the 6 low ones (1 to 64 pixels). Then, by kind:
//
//   0 skip      transparent pixels: nothing follows, nothing is drawn
//   1 opaque    pixels that cover: 2 bytes each follow
//   2 blended   partly transparent pixels (the smoothed edges): 2 bytes and
//               an alpha (1..254) each follow, blended over what is drawn
//   3 repeat    one pixel that covers, repeated: its 2 bytes follow
//
// A pixel is 16 bits, high byte first (5 bits of red, 6 of green, 5 of
// blue), as the targets of draw.h hold them: an opaque run is a copy.
//
// No allocation, no state: any thread. The file may come from a program:
// every offset is checked against the size of the file.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace draw {

// The kinds of runs (the 2 high bits of their first byte).
const int IMAGE_RUN_SKIP = 0;
const int IMAGE_RUN_OPAQUE = 1;
const int IMAGE_RUN_BLENDED = 2;
const int IMAGE_RUN_REPEAT = 3;

// An image file, its header read (openImage()).
struct Image {
    const uint8_t *data;           // the file; borrowed (in the package: flash on a device)
    size_t         size;           // bytes of the file
    int            width;          // pixels
    int            height;
};

/**
 * Reads the header of the image file 'data'. False when it is not a file of
 * this format (another one, another version, a table of rows out of the
 * file).
 */
bool openImage(const uint8_t *data, size_t size, Image *out);

/**
 * The runs of row 'row' (0 to height - 1): from *start to *end. False when
 * the row lies out of the file.
 */
bool imageRow(const Image *image, int row, const uint8_t **start, const uint8_t **end);

} // namespace draw
