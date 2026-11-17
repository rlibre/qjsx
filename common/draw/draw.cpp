/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Drawing in memory (draw.h): filled rectangles, lines, text, arcs, rounded
// rectangles, lists of commands.
//
// HOW IT IS MADE FAST (measured on the WT32-SC01, ESP32 at 240 MHz, a band of
// 16 rows of 480 pixels, 2026-11-17):
//
// - Rendering is the bottleneck, not the SPI wire: two gauges (448 x 250
//   pixels) went out in 91 ms for 45 ms of wire at 40 MHz, and asking
//   80 MHz saved 3 ms only (and the ST7796 shows nothing at 80 MHz).
//   Per image, the arcs took 38 ms, the rounded rectangles 37, the text 3.4,
//   the background of the bands 1.4 (counted by temporary code, since
//   removed).
//   After the changes below: arcs 19 ms, rounded rectangles 10, text 3.5,
//   background 1.4; 34 ms of rendering an image instead of 80, the gauges
//   at 15.5 images a second instead of 9.9, shown in 55 ms (45 of wire).
//   What did it: rounded rectangles by spans, -17 ms; arcs in 32-bit
//   integers, -18 ms (a first fixed point version with 64-bit integers
//   gained nothing: on the ESP32 a 64-bit multiply or compare is a library
//   call, as slow as the float it replaced).
//
// - Work only where it matters: an arc skips the hole of its ring (a ring is
//   mostly hole); a rounded rectangle fills its straight parts as they are
//   and computes a coverage only in its corners; a text is skipped by the
//   bands its line does not reach.
//
// - Fixed point, no float per pixel (user, 2026-11-17): coverages are 0 to
//   COVERED (256), distances 24.8 (256 = a pixel), directions 2.14. Reasons:
//   some chips have no floating point unit (ESP32-C3 / C6: every float
//   operation is a call), and on the ESP32 itself square roots, divisions
//   and int <-> float conversions are done in software. Blending divides by
//   256 with a shift instead of dividing by 255.
//
// - Square roots only at the edges: a pixel is classified by its squared
//   distance (exact integers, times 4 so that half pixels stay whole) as
//   outside, fully covered, or on a one pixel wide edge; only the edge
//   pixels need a distance. That one is exact: squareRoot() bit by bit (no
//   float, no division, 32 bits only). Considered and not taken until a measure asks for
//   it: Quake III's fast inverse square root (the 0x5f3759df trick: an
//   approximation of 1 / sqrt on a float, about 0.2 % off after one Newton
//   step; quick with a floating point unit, a software float without one),
//   and an integer Newton step started from the radius (edge pixels are
//   within a pixel of it).
#include "draw.h"

#include <math.h>
#include <string.h>

namespace draw {

namespace {

/**
 * The smaller of two integers.
 */
int lowest(int a, int b) {
    return a < b ? a : b;
}

/**
 * The larger of two integers.
 */
int highest(int a, int b) {
    return a > b ? a : b;
}

/**
 * The absolute value of an integer.
 */
int distance(int value) {
    return value < 0 ? -value : value;
}

/**
 * The code point at units[*index], moved past it: a surrogate pair is one
 * code point, a lone surrogate is itself.
 */
uint32_t nextCodePoint(const uint16_t *units, int count, int *index) {
    uint32_t first = units[*index];
    *index += 1;

    // a high surrogate followed by a low one
    bool isHigh = first >= 0xD800 && first <= 0xDBFF;
    bool hasLow = *index < count && units[*index] >= 0xDC00 && units[*index] <= 0xDFFF;
    if (isHigh && hasLow) {
        uint32_t second = units[*index];
        *index += 1;
        return 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
    }

    return first;
}

// Coverage of a pixel, in fixed point: 0 (nothing) to COVERED (all of it).
// Distances are in 24.8 fixed point (256 = one pixel). No float per pixel:
// some chips have no floating point unit, and the ESP32 computes square
// roots and divisions in software.
const int COVERED = 256;
const int ONE_PIXEL = 256;
const int HALF_PIXEL = 128;

// Directions are unit vectors in 2.14 fixed point.
const int DIRECTION_ONE = 16384;
const int DIRECTION_TO_DISTANCE_SHIFT = 6;   // 2.14 x pixels -> 24.8

/**
 * 'value' kept within 0..COVERED.
 */
int clampCoverage(int value) {
    return value < 0 ? 0 : value > COVERED ? COVERED : value;
}

/**
 * The integer square root of a 32-bit value (rounded down), bit by bit: no
 * float, no division, no 64-bit arithmetic (a library call on the ESP32).
 */
uint32_t squareRoot(uint32_t value) {
    // nothing to do for 0
    if (value == 0) {
        return 0;
    }

    // the highest power of four not above the value, from its highest bit
    int highestBit = 31 - __builtin_clz(value);
    uint32_t bit = (uint32_t)1 << (highestBit & ~1);
    uint32_t root = 0;

    // each bit of the root, from the highest
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }

    return root;
}

/**
 * The square root, in 24.8 fixed point, of a squared distance with
 * 'fraction' bits of fraction (an even number, at most 16): the root of the
 * value shifted left so that the root has 8 bits of fraction; shifted less
 * (a bit of precision less per 2 bits) when that would not fit in 32 bits.
 */
int rootTo24_8(uint32_t squared, int fraction) {
    // as much of the shift as fits
    int shift = 16 - fraction;
    while (shift > 0 && squared > (0xFFFFFFFFu >> shift)) {
        shift -= 2;
    }

    // the root, then what the shift lacked
    uint32_t root = squareRoot(squared << shift);
    return (int)(root << ((16 - fraction - shift) / 2));
}

/**
 * Pixel 'background' with 'foreground' over it at 'coverage' (0 to
 * COVERED; both 16-bit pixels: 5 bits of red, 6 of green, 5 of blue).
 */
uint16_t blend(uint16_t background, uint16_t foreground, int coverage) {
    // each channel, moved toward the foreground's by the coverage
    int red = (background >> 11) & 0x1F;
    int green = (background >> 5) & 0x3F;
    int blue = background & 0x1F;
    red += ((((foreground >> 11) & 0x1F) - red) * coverage) >> 8;
    green += ((((foreground >> 5) & 0x3F) - green) * coverage) >> 8;
    blue += (((foreground & 0x1F) - blue) * coverage) >> 8;

    return (uint16_t)((red << 11) | (green << 5) | blue);
}

/**
 * Writes 'pixel' over the target's pixel at 'at' with 'coverage' (0 to
 * COVERED).
 */
void putCoverage(uint8_t *at, uint16_t pixel, int coverage) {
    // not covered: nothing
    if (coverage <= 0) {
        return;
    }

    // covered: the color; partly: blended with what is there
    uint16_t under = (uint16_t)((at[0] << 8) | at[1]);
    uint16_t mixed = coverage >= COVERED ? pixel : blend(under, pixel, coverage);
    at[0] = (uint8_t)(mixed >> 8);
    at[1] = (uint8_t)mixed;
}

/**
 * Draws one glyph, the left of its pen at 'penX' and its baseline at
 * 'baseline' (both in the target's own pixels), blended over the target.
 */
void drawGlyph(const Target *target, const Font *font, const Glyph *glyph, int penX, int baseline, uint16_t pixel) {
    // its box in the target
    int left = penX + glyph->x;
    int top = baseline - glyph->y - glyph->height;
    int full = (1 << font->bpp) - 1;

    // the coverage of each value of a glyph's pixel (no division per pixel;
    // 8 bits per pixel: computed, the table would be too large)
    int16_t levels[16];
    bool hasTable = font->bpp <= 4;
    if (hasTable) {
        for (int value = 0; value <= full; value++) {
            levels[value] = (int16_t)(value * COVERED / full);
        }
    }

    // its rows in the target only (a band holds a few)
    int firstRow = highest(0, -top);
    int lastRow = lowest(glyph->height, target->height - top);

    for (int row = firstRow; row < lastRow; row++) {
        uint8_t *line = target->bytes + (size_t)(top + row) * (size_t)target->width * 2;

        for (int column = 0; column < glyph->width; column++) {
            // a pixel of the target, covered at all
            int x = left + column;
            if (x < 0 || x >= target->width) {
                continue;
            }

            int value = glyphPixel(font, glyph, (uint32_t)(row * glyph->width + column));
            int coverage = hasTable ? levels[value] : value + (value >> 7);
            putCoverage(line + (size_t)x * 2, pixel, coverage);
        }
    }
}

/**
 * The unit vector of an angle in tenths of a degree, 0 at the top,
 * clockwise, in the target's axes (y goes down), in 2.14 fixed point
 * (computed once per arc).
 */
void directionOf(int tenths, int *x, int *y) {
    float radians = (float)tenths * 3.14159265f / 1800.0f;
    *x = (int)lroundf(sinf(radians) * (float)DIRECTION_ONE);
    *y = (int)lroundf(-cosf(radians) * (float)DIRECTION_ONE);
}

// A rectangle with rounded corners, in the target's own axes, in 24.8 fixed
// point: the pixel at column c covers c to c + 1.
struct RoundBox {
    int centerX;                 // its center
    int centerY;
    int halfWidth;               // half its size
    int halfHeight;
    int radius;                  // of its corners
};

/**
 * How much of pixel (column, row) a rounded box covers, 0 to COVERED: from
 * the signed distance of the pixel's center to its edge (one pixel of
 * smoothing).
 */
int boxCoverage(const RoundBox *box, int column, int row) {
    // from the center, folded into one corner, past the straight part
    int qx = distance(column * ONE_PIXEL + HALF_PIXEL - box->centerX) - (box->halfWidth - box->radius);
    int qy = distance(row * ONE_PIXEL + HALF_PIXEL - box->centerY) - (box->halfHeight - box->radius);

    // outside the straight part: the distance to the corner's circle (from
    // 24.4 values: the sum of their squares has 8 bits of fraction and fits
    // 32 bits for corners up to 2047 pixels); inside: to the nearer side
    uint32_t outsideX = (uint32_t)highest(qx, 0) >> 4;
    uint32_t outsideY = (uint32_t)highest(qy, 0) >> 4;
    int outside = rootTo24_8(outsideX * outsideX + outsideY * outsideY, 8);
    int signedDistance = outside + lowest(highest(qx, qy), 0) - box->radius;
    return clampCoverage(HALF_PIXEL - signedDistance);
}

/**
 * Fills columns 'from' to 'to' (excluded) of a row of the target with
 * 'pixel', cut to the target.
 */
void fillRow(const Target *target, int row, int from, int to, uint16_t pixel) {
    int first = highest(from, 0);
    int last = lowest(to, target->width);
    uint8_t *at = target->bytes + ((size_t)row * (size_t)target->width + (size_t)first) * 2;
    uint8_t high = (uint8_t)(pixel >> 8);
    uint8_t low = (uint8_t)pixel;

    for (int column = first; column < last; column++) {
        at[0] = high;
        at[1] = low;
        at += 2;
    }
}

} // namespace

void fillRect(const Target *target, int x, int y, int width, int height, uint16_t pixel) {
    // the part of the rectangle in the target, in the target's own pixels
    int left = highest(0, x - target->originX);
    int top = highest(0, y - target->originY);
    int right = lowest(target->width, x + width - target->originX);
    int bottom = lowest(target->height, y + height - target->originY);
    if (right <= left || bottom <= top) {
        return;
    }

    // its first row, pixel by pixel
    size_t stride = (size_t)target->width * 2;
    size_t rowBytes = (size_t)(right - left) * 2;
    uint8_t *first = target->bytes + (size_t)top * stride + (size_t)left * 2;
    uint8_t high = (uint8_t)(pixel >> 8);
    uint8_t low = (uint8_t)pixel;

    for (size_t offset = 0; offset < rowBytes; offset += 2) {
        first[offset] = high;
        first[offset + 1] = low;
    }

    // its other rows: copies of the first
    for (int row = top + 1; row < bottom; row++) {
        memcpy(first + (size_t)(row - top) * stride, first, rowBytes);
    }
}

void line(const Target *target, int x0, int y0, int x1, int y1, uint16_t pixel) {
    // in the target's own pixels
    int x = x0 - target->originX;
    int y = y0 - target->originY;
    int endX = x1 - target->originX;
    int endY = y1 - target->originY;

    // entirely on one side of the target: nothing (most lines, for a band)
    bool isBeside = highest(x, endX) < 0 || lowest(x, endX) >= target->width;
    bool isAboveOrBelow = highest(y, endY) < 0 || lowest(y, endY) >= target->height;
    if (isBeside || isAboveOrBelow) {
        return;
    }

    // how far it goes each way, and in which direction
    int spanX = distance(endX - x);
    int spanY = distance(endY - y);
    int stepX = x < endX ? 1 : -1;
    int stepY = y < endY ? 1 : -1;
    int width = target->width;
    int height = target->height;
    uint8_t *bytes = target->bytes;
    uint8_t high = (uint8_t)(pixel >> 8);
    uint8_t low = (uint8_t)pixel;

    // Bresenham: 'error' tells which of x and y moves next
    int error = spanX - spanY;

    for (;;) {
        // this pixel, when in the target
        bool isInside = x >= 0 && x < width && y >= 0 && y < height;
        if (isInside) {
            size_t offset = ((size_t)y * (size_t)width + (size_t)x) * 2;
            bytes[offset] = high;
            bytes[offset + 1] = low;
        }

        // the end
        if (x == endX && y == endY) {
            break;
        }

        // the next pixel
        int doubled = 2 * error;
        if (doubled > -spanY) {
            error -= spanY;
            x += stepX;
        }

        if (doubled < spanX) {
            error += spanX;
            y += stepY;
        }
    }
}

void text(const Target *target, const Font *font, int x, int y, const uint16_t *units, int count, uint16_t pixel, int flags) {
    // the baseline in the target's own pixels
    int baseline = y + font->ascent - target->originY;

    // no glyph can reach the target's rows: nothing to do (most texts, for
    // a band)
    bool isAbove = baseline - font->minY < 0;
    bool isBelow = baseline - font->maxY >= target->height;
    if (isAbove || isBelow) {
        return;
    }

    // the pen moves in 1/16 px, from the left of the line
    bool tabularDigits = (flags & TEXT_TABULAR_DIGITS) != 0;
    int pen = (x - target->originX) * 16;

    for (int index = 0; index < count;) {
        // the glyph of the next code point; skipped when the font lacks it
        uint32_t codePoint = nextCodePoint(units, count, &index);
        Glyph glyph;
        if (!glyphOf(font, glyphIdOf(font, codePoint), &glyph)) {
            continue;
        }

        // a tabular digit: centered in the widest digit's width
        int advance = glyph.advance;
        int shift = 0;
        bool isDigit = codePoint >= '0' && codePoint <= '9';
        if (tabularDigits && isDigit && font->digitAdvance > 0) {
            shift = (font->digitAdvance - advance) / 2;
            advance = font->digitAdvance;
        }

        // drawn at the pen, which moves on
        drawGlyph(target, font, &glyph, (pen + shift) >> 4, baseline, pixel);
        pen += advance;
    }
}

void arc(const Target *target, int cx, int cy, int radius, int thickness, int start, int end, uint16_t pixel) {
    // an arc at all: some sweep, some radius and thickness
    int sweep = end - start;
    if (sweep <= 0 || radius < 0 || thickness < 1) {
        return;
    }

    // the ring: outer and inner radius (a disc when thick enough)
    int inner = radius - thickness;
    bool isDisc = inner < 0;

    // its rows in the target only (a band holds a few)
    int centerX = cx - target->originX;
    int centerY = cy - target->originY;
    int firstRow = highest(0, centerY - radius);
    int lastRow = lowest(target->height - 1, centerY + radius);
    if (firstRow > lastRow) {
        return;
    }

    // its straight ends: the directions of the start and of the end
    bool isWhole = sweep >= 3600;
    bool isWide = sweep > 1800;
    int startX = 0;
    int startY = 0;
    int endX = 0;
    int endY = 0;
    directionOf(start, &startX, &startY);
    directionOf(end, &endX, &endY);

    // squared distances (times 4: half pixels stay whole) that need no
    // square root: from "outside" nothing is covered; between "fullFrom"
    // and "fullTo" the ring covers the pixel entirely (only the one pixel
    // wide edges are smoothed)
    // (32 bits: radii up to about 20,000 pixels)
    int32_t outside = (2 * radius + 1) * (2 * radius + 1);
    int32_t fullTo = radius > 0 ? (2 * radius - 1) * (2 * radius - 1) : 0;
    int32_t fullFrom = isDisc ? 0 : (2 * inner + 1) * (2 * inner + 1);
    int hole = inner - 1;

    for (int row = firstRow; row <= lastRow; row++) {
        // the pixels of the row the outer circle crosses
        int dy = row - centerY;
        int reach = (int)squareRoot((uint32_t)highest(radius * radius - dy * dy, 0));
        int firstColumn = highest(0, centerX - reach - 1);
        int lastColumn = lowest(target->width - 1, centerX + reach + 1);
        uint8_t *line = target->bytes + (size_t)row * (size_t)target->width * 2;

        // the pixels of the row in the hole, which nothing covers: skipped
        // (a ring is mostly hole)
        int holeReach = -1;
        bool crossesHole = !isDisc && hole > distance(dy);
        if (crossesHole) {
            holeReach = (int)squareRoot((uint32_t)(hole * hole - dy * dy));
        }

        for (int column = firstColumn; column <= lastColumn; column++) {
            // in the hole: on to its other side
            int dx = column - centerX;
            if (dx >= -holeReach && dx <= holeReach) {
                column = centerX + holeReach;
                continue;
            }

            // how much of the pixel the ring covers: entirely within its
            // edges, else from the distance to its outer and inner edges
            int32_t squared = dx * dx + dy * dy;
            int32_t squaredTimesFour = squared * 4;
            if (squaredTimesFour >= outside) {
                continue;
            }

            int covered = COVERED;
            bool isFull = squaredTimesFour <= fullTo && squaredTimesFour >= fullFrom;
            if (!isFull) {
                int fromCenter = rootTo24_8((uint32_t)squared, 0);
                covered = clampCoverage(radius * ONE_PIXEL + HALF_PIXEL - fromCenter);
                if (!isDisc) {
                    covered = lowest(covered, clampCoverage(fromCenter - inner * ONE_PIXEL + HALF_PIXEL));
                }
            }

            // and the sweep: the signed distances to the start and end lines
            // (beyond the start, before the end); a sweep over a half turn
            // is the outside of the other part
            if (!isWhole && covered > 0) {
                int afterStart = clampCoverage(((startX * dy - startY * dx) >> DIRECTION_TO_DISTANCE_SHIFT) + HALF_PIXEL);
                int beforeEnd = clampCoverage(((dx * endY - dy * endX) >> DIRECTION_TO_DISTANCE_SHIFT) + HALF_PIXEL);
                int swept = isWide ? highest(afterStart, beforeEnd) : lowest(afterStart, beforeEnd);
                covered = (covered * swept) >> 8;
            }

            // blended over the pixel there
            putCoverage(line + (size_t)column * 2, pixel, covered);
        }
    }
}

/**
 * Draws columns "from" to "to" (excluded) of a row of a rounded rectangle,
 * pixel by pixel: the outer box's coverage, less the inner box's (the hole
 * of an outline; nullptr: filled) in its columns "holeFrom" to "holeTo".
 * Used where an edge is smoothed: in the corners.
 */
void roundEdges(const Target *target, int row, int from, int to, const RoundBox *outer, const RoundBox *inner, int holeFrom, int holeTo, uint16_t pixel) {
    int first = highest(from, 0);
    int last = lowest(to, target->width);
    uint8_t *line = target->bytes + (size_t)row * (size_t)target->width * 2;

    for (int column = first; column < last; column++) {
        // the outer box, less the hole
        int covered = boxCoverage(outer, column, row);
        bool isHoleColumn = inner != nullptr && column >= holeFrom && column < holeTo;
        if (isHoleColumn && covered > 0) {
            covered = (covered * (COVERED - boxCoverage(inner, column, row))) >> 8;
        }

        putCoverage(line + (size_t)column * 2, pixel, covered);
    }
}

void roundRect(const Target *target, int x, int y, int width, int height, int radius, int thickness, uint16_t pixel) {
    // a rectangle at all; the radius at most half its smaller side
    if (width < 1 || height < 1) {
        return;
    }

    int smaller = lowest(width, height);
    int corner = lowest(highest(radius, 0), smaller / 2);

    // filled when no thickness, or one that reaches the middle
    bool isFilled = thickness <= 0 || thickness * 2 >= smaller;
    int innerCorner = highest(corner - thickness, 0);

    // in the target's own pixels: the outer box, and the hole of an outline
    // (24.8)
    int left = x - target->originX;
    int top = y - target->originY;
    int right = left + width;
    RoundBox outer = { left * ONE_PIXEL + width * HALF_PIXEL, top * ONE_PIXEL + height * HALF_PIXEL,
                       width * HALF_PIXEL, height * HALF_PIXEL, corner * ONE_PIXEL };
    RoundBox inner = { outer.centerX, outer.centerY, outer.halfWidth - thickness * ONE_PIXEL,
                       outer.halfHeight - thickness * ONE_PIXEL, innerCorner * ONE_PIXEL };
    const RoundBox *hole = isFilled ? nullptr : &inner;

    // its rows in the target only
    int firstRow = highest(0, top);
    int lastRow = lowest(target->height, top + height);

    for (int row = firstRow; row < lastRow; row++) {
        // where the row is: in the rows of the outer corners, in the hole
        // of an outline, in the rows of its corners
        bool isOuterCornerRow = row < top + corner || row >= top + height - corner;
        bool isHoleRow = !isFilled && row >= top + thickness && row < top + height - thickness;
        bool isInnerCornerRow = isHoleRow && (row < top + thickness + innerCorner || row >= top + height - thickness - innerCorner);

        // a row with no hole: smoothed in its corners, straight between
        if (!isHoleRow) {
            if (isOuterCornerRow) {
                roundEdges(target, row, left, left + corner, &outer, nullptr, 0, 0, pixel);
                fillRow(target, row, left + corner, right - corner, pixel);
                roundEdges(target, row, right - corner, right, &outer, nullptr, 0, 0, pixel);
            } else {
                fillRow(target, row, left, right, pixel);
            }
            continue;
        }

        // a row of the hole: its two sides only, smoothed where a corner
        // (outer or of the hole) crosses the row
        bool hasCorner = isOuterCornerRow || isInnerCornerRow;
        if (hasCorner) {
            int side = thickness + (isInnerCornerRow ? innerCorner : 0);
            roundEdges(target, row, left, left + side, &outer, hole, left + thickness, right - thickness, pixel);
            roundEdges(target, row, right - side, right, &outer, hole, left + thickness, right - thickness, pixel);
        } else {
            fillRow(target, row, left, left + thickness, pixel);
            fillRow(target, row, right - thickness, right, pixel);
        }
    }
}

int textWidth(const Font *font, const uint16_t *units, int count, int flags) {
    // the advances of its code points, in 1/16 px
    bool tabularDigits = (flags & TEXT_TABULAR_DIGITS) != 0;
    int width = 0;

    for (int index = 0; index < count;) {
        uint32_t codePoint = nextCodePoint(units, count, &index);
        width += advanceOf(font, codePoint, tabularDigits);
    }

    // in whole pixels, rounded up
    return (width + 15) >> 4;
}

void render(const int16_t *items, int count, const Font *fonts, int fontCount, const Target *target) {
    for (int at = 0; at + COMMAND_ITEMS <= count;) {
        // the next command, and the items it takes
        const Command *command = (const Command *)(items + at);
        int taken = COMMAND_ITEMS;
        if (command->kind == KIND_TEXT) {
            taken += command->y1 > 0 ? command->y1 : 0;
        } else if (command->kind == KIND_ARC) {
            taken += ARC_EXTRA_ITEMS;
        } else if (command->kind == KIND_ROUND_RECT) {
            taken += ROUND_RECT_EXTRA_ITEMS;
        }

        // cut by the end of the items: the list stops there
        if (taken > count - at) {
            break;
        }

        switch (command->kind) {
            case KIND_FILL_RECT: {
                fillRect(target, command->x0, command->y0, command->x1, command->y1, command->pixel);
                break;
            }
            case KIND_LINE: {
                line(target, command->x0, command->y0, command->x1, command->y1, command->pixel);
                break;
            }
            case KIND_TEXT: {
                // its font, given; its characters follow it
                int fontIndex = command->x1 & 0x0F;
                int flags = (command->x1 >> 4) & 0x0FFF;
                const uint16_t *units = (const uint16_t *)(items + at + COMMAND_ITEMS);
                if (fontIndex < fontCount) {
                    text(target, &fonts[fontIndex], command->x0, command->y0, units, taken - COMMAND_ITEMS, command->pixel, flags);
                }
                break;
            }
            case KIND_ARC: {
                // its angles follow it
                const int16_t *angles = items + at + COMMAND_ITEMS;
                arc(target, command->x0, command->y0, command->x1, command->y1, angles[0], angles[1], command->pixel);
                break;
            }
            case KIND_ROUND_RECT: {
                // its radius and thickness follow it
                const int16_t *corner = items + at + COMMAND_ITEMS;
                roundRect(target, command->x0, command->y0, command->x1, command->y1, corner[0], corner[1], command->pixel);
                break;
            }
            default: {
                // not a command: nothing
                break;
            }
        }

        at += taken;
    }
}

} // namespace draw
