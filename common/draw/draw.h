/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Drawing in memory, for the displays (stdlib/display through sys/draw,
// draw_module.cpp; 2026-11-17, tried on the WT32-SC01).
//
// Pixels are 2 bytes each, high byte first (5 bits of red, 6 of green, 5 of
// blue): what the display controllers take over SPI.
//
// A "target" is a rectangle of rows of pixels in memory, placed in the
// coordinates of the drawing by its origin: a whole canvas (origin 0, 0), or
// a few rows of a larger drawing (a band). The drawings are cut to it.
//
// A list of commands (Command) is a drawing kept as what to draw, not as
// pixels: render() draws it into a target, so a large drawing can be made
// band after band in a small buffer, with no memory for the whole of it.
// The idea of a display list rendered a few rows at a time comes from Poco,
// the renderer of the Moddable SDK (https://github.com/Moddable-OpenSource/moddable).
// No code of it is used: this is our own implementation.
//
// No JS here, no allocation, no state: these functions can run on any
// thread (a rendering task later).
#pragma once

#include <stdint.h>

#include "font.h"
#include "image.h"

namespace draw {

// Rows of pixels in memory, placed in the drawing.
struct Target {
    uint8_t *bytes;              // row by row, 2 bytes per pixel; borrowed
    int      width;              // pixels per row
    int      height;             // rows
    int      originX;            // where its first pixel is in the drawing
    int      originY;
};

// What a command draws (Command.kind).
const int16_t KIND_FILL_RECT = 0;    // x0, y0: its first pixel; x1, y1: its width and height
const int16_t KIND_LINE = 1;         // from x0, y0 to x1, y1, both included
const int16_t KIND_TEXT = 2;         // x0, y0: the top left of its line; x1: font index (low 4 bits)
                                     // and TEXT_... flags (the 12 bits above); y1: its length in UTF-16 units,
                                     // which follow the command, one item each
const int16_t KIND_ARC = 3;          // x0, y0: its center; x1: its outer radius; y1: its thickness
                                     // (toward the center); then 2 items: start and end angles in
                                     // tenths of a degree, 0 at the top, clockwise

const int16_t KIND_ROUND_RECT = 4;   // x0, y0: its first pixel; x1, y1: its width and height; then
                                     // 2 items: the radius of its corners, its thickness (0: filled)
const int16_t KIND_IMAGE = 5;        // x0, y0: its top left pixel; x1: image index; y1 and pixel unused

// Items an arc and a rounded rectangle take after their command.
const int ARC_EXTRA_ITEMS = 2;
const int ROUND_RECT_EXTRA_ITEMS = 2;

// Flags of a text (bits 4 and above of its x1), and the most fonts a list
// may use (its font index is 4 bits).
const int TEXT_TABULAR_DIGITS = 1;   // the digits 0..9 all take the width of the widest one
const int MAX_FONTS = 16;

// The most images a list may use.
const int MAX_IMAGES = 16;

// One command of a list: 6 values of 16 bits, so that a list is exactly an
// Int16Array a JS program writes itself, 6 items per command, a text
// followed by its characters (no call per command: a call from JS to native
// code costs about 0.25 ms on an ESP32).
struct Command {
    int16_t  kind;               // what it draws: KIND_...; another value: nothing
    uint16_t pixel;              // its color, as the 16 bits of a pixel
    int16_t  x0;                 // see KIND_...
    int16_t  y0;
    int16_t  x1;
    int16_t  y1;
};

// Items (of 16 bits) of a command, its characters not counted.
const int COMMAND_ITEMS = 6;

/**
 * Fills a rectangle with one color; what is outside the target is not drawn.
 */
void fillRect(const Target *target, int x, int y, int width, int height, uint16_t pixel);

/**
 * Draws a line from (x0, y0) to (x1, y1), both included, one pixel thick;
 * what is outside the target is not drawn.
 */
void line(const Target *target, int x0, int y0, int x1, int y1, uint16_t pixel);

/**
 * Draws a line of text with 'font', the top left of its line at (x, y):
 * its baseline is 'ascent' pixels below. The glyphs are blended over what
 * the target holds (their pixels are coverage, 0 to 2^bpp - 1). A code point
 * the font does not have is skipped. 'units' are UTF-16 code units: a
 * surrogate pair is one code point.
 */
void text(const Target *target, const Font *font, int x, int y, const uint16_t *units, int count, uint16_t pixel, int flags);

/**
 * Draws an arc of a ring centered on pixel (cx, cy): from 'radius' (its
 * outer edge) inward over 'thickness' pixels (radius or more: no hole, a
 * filled sector on part of a turn, a disc on a whole one: user, 2026-11-17), from
 * angle 'start' to 'end' in tenths of a degree, 0 at the top, clockwise (an
 * end at or before the start: nothing; 360 degrees or more: the whole ring).
 * Its edges, round and straight, are smoothed: blended over the target.
 */
void arc(const Target *target, int cx, int cy, int radius, int thickness, int start, int end, uint16_t pixel);

/**
 * Draws a rectangle with rounded corners, its first pixel at (x, y): filled
 * ('thickness' 0, or at least half its smaller side), or its outline
 * 'thickness' pixels wide inside it. The same 'radius' at the four corners,
 * at most half the smaller side. The corners are smoothed (blended over the
 * target); the straight parts are filled as they are.
 */
void roundRect(const Target *target, int x, int y, int width, int height, int radius, int thickness, uint16_t pixel);

/**
 * Draws an image, its top left pixel at (x, y): its opaque pixels copied,
 * its partly transparent ones blended over the target, its transparent ones
 * left as they are. A row whose runs go out of the file stops there.
 */
void drawImage(const Target *target, const Image *image, int x, int y);

/**
 * The width of a line of text, in whole pixels (rounded up), as text()
 * draws it.
 */
int textWidth(const Font *font, const uint16_t *units, int count, int flags);

/**
 * Draws the commands of 'items' (count items of 16 bits: commands, a text
 * followed by its characters) into the target, in their order (the later
 * ones over the earlier ones). A text uses fonts[its font index], an image
 * images[its image index]; one whose font or image is not given, or a
 * command cut by the end of the items, is skipped.
 */
void render(const int16_t *items, int count, const Font *fonts, int fontCount, const Image *images, int imageCount,
            const Target *target);

} // namespace draw
