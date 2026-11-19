/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Fonts of the renderer (draw.h): files in LVGL's binary bitmap font format,
// as `lv_font_conv --format bin --no-compress` makes them (tools/fonts/; the
// format is described in that tool's doc/font_spec.md, MIT). Read IN PLACE:
// on a device the file lies in the package, in flash, and nothing of it is
// copied.
//
// What this reader takes: no compression (compression 0), no subpixel
// rendering, 1, 2, 3, 4 or 8 bits per pixel, character maps of the four
// formats, glyph offsets of 16 or 32 bits. The kerning table, when there is
// one, is ignored. What the format leaves unsaid was settled on a file made
// by the tool (temp/fonts/inter/parse.js, 2026-11-17): the glyph bits are
// read most significant first, the offsets of the glyphs count from the
// start of the 'glyf' table, the box of a glyph has a signed x and y.
//
// No allocation, no state: any thread. The file may come from a program:
// every offset is checked against the size of the file.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace draw {

// A font file, its tables found and its header read (openFont()).
struct Font {
    const uint8_t *data;           // the file; borrowed (in the package: flash on a device)
    size_t         size;           // bytes of the file
    const uint8_t *cmap;           // the 'cmap' table: code points to glyphs
    uint32_t       cmapSize;       // its bytes
    const uint8_t *loca;           // the 'loca' table: where each glyph is in 'glyf'
    uint32_t       locaSize;       // its bytes
    const uint8_t *glyf;           // the 'glyf' table: the glyphs
    uint32_t       glyfSize;       // its bytes
    uint32_t       glyphCount;     // entries of 'loca'
    int            pixelSize;      // size of the font, pixels
    int            ascent;         // pixels above the baseline of a line
    int            descent;        // pixels below it, 0 or negative
    int            minY;           // lowest pixel of a glyph, from the baseline (up is positive)
    int            maxY;           // highest one
    int            locaWide;       // 1: offsets of 32 bits, 0: of 16 bits
    int            advanceFormat;  // 0: whole pixels, 1: 4 bits of fraction
    int            defaultAdvance; // advance of every glyph when advanceBits is 0
    int            bpp;            // bits per pixel of a glyph: 1, 2, 3, 4 or 8
    int            xyBits;         // bits of the x and y of a glyph's box
    int            whBits;         // bits of its width and height
    int            advanceBits;    // bits of its advance
    int            digitAdvance;   // advance of the widest digit 0..9, 1/16 px (tabular digits)
};

// One glyph of a font (glyphOf()).
struct Glyph {
    int            advance;        // how far the pen moves, 1/16 px
    int            x;              // left of its box from the pen, pixels
    int            y;              // bottom of its box from the baseline, pixels, up is positive
    int            width;          // its box, pixels
    int            height;
    const uint8_t *bits;           // its stream of bits (in the file)
    uint32_t       firstBit;       // where its first pixel starts in 'bits'
};

/**
 * Finds the tables of the font file 'data' and reads its header. False when
 * it is not a file this reader takes (another format, compressed, subpixel,
 * a table out of the file).
 */
bool openFont(const uint8_t *data, size_t size, Font *out);

/**
 * The glyph id of a code point; 0 when the font does not have it.
 */
uint32_t glyphIdOf(const Font *font, uint32_t codePoint);

/**
 * Reads glyph 'id'. False when there is no such glyph or its bits go past
 * the end of the file.
 */
bool glyphOf(const Font *font, uint32_t id, Glyph *out);

/**
 * How far the pen moves for a code point, 1/16 px (0 when the font does not
 * have it); with 'tabularDigits', every digit 0..9 moves as the widest one.
 */
int advanceOf(const Font *font, uint32_t codePoint, bool tabularDigits);

/**
 * The height of the capital letters, pixels above the baseline: the top of
 * the glyph of 'H' (what a text is centered on to look centered: a line also
 * holds the room of the accents and of the descenders). A font without 'H':
 * the top of its '0'; without digits either: its ascent.
 */
int capHeightOf(const Font *font);

/**
 * The value of pixel 'index' of a glyph, 0 (nothing) to 2^bpp - 1 (full).
 */
int glyphPixel(const Font *font, const Glyph *glyph, uint32_t index);

} // namespace draw
