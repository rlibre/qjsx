/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Fonts in LVGL's binary bitmap font format, read in place (font.h).
#include "font.h"

namespace draw {

namespace {

// Bytes of the start of every table: its size (4) and its tag (4).
const uint32_t TABLE_HEADER = 8;

// Bytes of the 'head' table this reader needs.
const uint32_t HEAD_SIZE = 44;

// The start of the character maps: table header, then the count (4).
const uint32_t CMAP_FIRST_SUBTABLE = 12;

// Bytes of the header of one character map.
const uint32_t CMAP_SUBTABLE_SIZE = 16;

// The start of the offsets of 'loca': table header, then the count (4).
const uint32_t LOCA_FIRST_OFFSET = 12;

// The formats of a character map.
const int CMAP_FULL = 0;                // one byte per code point of its range
const int CMAP_SPARSE = 1;              // listed code points, a glyph each
const int CMAP_FULL_TINY = 2;           // its range, glyphs in order
const int CMAP_SPARSE_TINY = 3;         // listed code points, glyphs in order

/**
 * A little-endian unsigned 16-bit value (read byte by byte: an unaligned
 * 32-bit read faults on some chips).
 */
uint32_t u16(const uint8_t *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8);
}

/**
 * A little-endian signed 16-bit value.
 */
int s16(const uint8_t *at) {
    return (int)(int16_t)u16(at);
}

/**
 * A little-endian unsigned 32-bit value.
 */
uint32_t u32(const uint8_t *at) {
    return u16(at) | (u16(at + 2) << 16);
}

/**
 * Whether the 4 bytes at 'at' are the tag 'tag'.
 */
bool isTag(const uint8_t *at, const char *tag) {
    return at[0] == tag[0] && at[1] == tag[1] && at[2] == tag[2] && at[3] == tag[3];
}

/**
 * Reads 'count' bits from 'bits', most significant first, from bit '*position'
 * (moved past them).
 */
uint32_t readBits(const uint8_t *bits, uint32_t *position, int count) {
    uint32_t value = 0;

    for (int i = 0; i < count; i++) {
        uint32_t at = *position;
        uint32_t bit = (bits[at >> 3] >> (7 - (at & 7))) & 1;
        value = (value << 1) | bit;
        *position = at + 1;
    }

    return value;
}

/**
 * 'count' bits read as a signed value (two's complement).
 */
int readSignedBits(const uint8_t *bits, uint32_t *position, int count) {
    uint32_t value = readBits(bits, position, count);
    bool isNegative = count > 0 && (value & (1u << (count - 1))) != 0;
    return isNegative ? (int)value - (1 << count) : (int)value;
}

/**
 * The glyph id 'codePoint' has in character map 'index', 0 when not in it.
 */
uint32_t glyphInSubtable(const Font *font, uint32_t index, uint32_t codePoint) {
    // its header
    const uint8_t *header = font->cmap + CMAP_FIRST_SUBTABLE + index * CMAP_SUBTABLE_SIZE;
    uint32_t dataOffset = u32(header);
    uint32_t start = u32(header + 4);
    uint32_t length = u16(header + 8);
    uint32_t glyphOffset = u16(header + 10);
    uint32_t entries = u16(header + 12);
    int format = header[14];

    // within its range
    bool isInRange = codePoint >= start && codePoint < start + length;
    if (!isInRange) {
        return 0;
    }

    // its data (counted from the start of 'cmap'), checked against the table
    uint32_t delta = codePoint - start;
    const uint8_t *data = font->cmap + dataOffset;
    uint32_t dataBytes = dataOffset <= font->cmapSize ? font->cmapSize - dataOffset : 0;

    switch (format) {
        case CMAP_FULL_TINY: {
            return glyphOffset + delta;
        }
        case CMAP_FULL: {
            // one byte per code point; 0: not in the font
            if (delta >= dataBytes || data[delta] == 0) {
                return 0;
            }
            return glyphOffset + data[delta];
        }
        case CMAP_SPARSE:
        case CMAP_SPARSE_TINY: {
            // the listed code points (16 bits each, from 'start'), searched
            bool hasGlyphs = format == CMAP_SPARSE;
            uint32_t listBytes = entries * 2;
            uint32_t neededBytes = hasGlyphs ? listBytes * 2 : listBytes;
            if (neededBytes > dataBytes) {
                return 0;
            }

            for (uint32_t i = 0; i < entries; i++) {
                if (u16(data + i * 2) != delta) {
                    continue;
                }

                // its glyph: listed after the code points, or its rank
                uint32_t glyph = hasGlyphs ? u16(data + listBytes + i * 2) : i;
                return glyphOffset + glyph;
            }
            return 0;
        }
        default: {
            // a format this reader does not know
            return 0;
        }
    }
}

} // namespace

bool openFont(const uint8_t *data, size_t size, Font *out) {
    *out = Font{};
    out->data = data;
    out->size = size;

    // the tables, one after the other: [size][tag]...
    const uint8_t *head = nullptr;

    for (size_t at = 0; at + TABLE_HEADER <= size;) {
        uint32_t tableSize = u32(data + at);
        bool fits = tableSize >= TABLE_HEADER && tableSize <= size - at;
        if (!fits) {
            return false;
        }

        const uint8_t *table = data + at;
        if (isTag(table + 4, "head")) {
            head = tableSize >= HEAD_SIZE ? table : nullptr;
        } else if (isTag(table + 4, "cmap")) {
            out->cmap = table;
            out->cmapSize = tableSize;
        } else if (isTag(table + 4, "loca")) {
            out->loca = table;
            out->locaSize = tableSize;
        } else if (isTag(table + 4, "glyf")) {
            out->glyf = table;
            out->glyfSize = tableSize;
        }

        at += tableSize;
    }

    // the four tables this reader needs
    bool hasTables = head != nullptr && out->cmap != nullptr && out->loca != nullptr && out->glyf != nullptr;
    if (!hasTables) {
        return false;
    }

    // the header: sizes, then how the glyphs are written
    out->pixelSize = (int)u16(head + 14);
    out->ascent = (int)u16(head + 16);
    out->descent = s16(head + 18);
    out->minY = s16(head + 26);
    out->maxY = s16(head + 28);
    out->defaultAdvance = (int)u16(head + 30);
    out->locaWide = head[34];
    out->advanceFormat = head[36];
    out->bpp = head[37];
    out->xyBits = head[38];
    out->whBits = head[39];
    out->advanceBits = head[40];
    int compression = head[41];
    int subpixel = head[42];

    // what this reader draws: uncompressed glyphs, whole pixels
    bool isBpp = out->bpp == 1 || out->bpp == 2 || out->bpp == 3 || out->bpp == 4 || out->bpp == 8;
    bool isDrawable = compression == 0 && subpixel == 0 && isBpp && out->xyBits <= 16 && out->whBits <= 16 && out->advanceBits <= 24;
    if (!isDrawable) {
        return false;
    }

    // the glyph count, and room for its offsets in 'loca'
    if (out->locaSize < CMAP_FIRST_SUBTABLE || out->cmapSize < CMAP_FIRST_SUBTABLE) {
        return false;
    }

    out->glyphCount = u32(out->loca + 8);
    uint32_t offsetBytes = out->locaWide ? 4 : 2;
    uint32_t subtables = u32(out->cmap + 8);
    bool fitsLoca = out->glyphCount <= (out->locaSize - LOCA_FIRST_OFFSET) / offsetBytes;
    bool fitsCmap = subtables <= (out->cmapSize - CMAP_FIRST_SUBTABLE) / CMAP_SUBTABLE_SIZE;
    if (!fitsLoca || !fitsCmap) {
        return false;
    }

    // the widest digit, for tabular digits
    for (uint32_t digit = '0'; digit <= '9'; digit++) {
        int advance = advanceOf(out, digit, false);
        if (advance > out->digitAdvance) {
            out->digitAdvance = advance;
        }
    }

    return true;
}

uint32_t glyphIdOf(const Font *font, uint32_t codePoint) {
    // the first character map that has it
    uint32_t subtables = u32(font->cmap + 8);

    for (uint32_t i = 0; i < subtables; i++) {
        uint32_t id = glyphInSubtable(font, i, codePoint);
        if (id != 0) {
            return id;
        }
    }

    return 0;
}

bool glyphOf(const Font *font, uint32_t id, Glyph *out) {
    // a glyph of the font (0 is "none")
    if (id == 0 || id >= font->glyphCount) {
        return false;
    }

    // where it starts in 'glyf'
    const uint8_t *offsets = font->loca + LOCA_FIRST_OFFSET;
    uint32_t offset = font->locaWide ? u32(offsets + id * 4) : u16(offsets + id * 2);
    if (offset >= font->glyfSize) {
        return false;
    }

    // its header: advance, box
    const uint8_t *bits = font->glyf + offset;
    uint32_t position = 0;
    int advance = font->advanceBits > 0 ? (int)readBits(bits, &position, font->advanceBits) : font->defaultAdvance;
    out->advance = font->advanceFormat == 1 ? advance : advance * 16;
    out->x = readSignedBits(bits, &position, font->xyBits);
    out->y = readSignedBits(bits, &position, font->xyBits);
    out->width = (int)readBits(bits, &position, font->whBits);
    out->height = (int)readBits(bits, &position, font->whBits);
    out->bits = bits;
    out->firstBit = position;

    // its pixels within the table
    uint64_t lastBit = (uint64_t)position + (uint64_t)out->width * (uint64_t)out->height * (uint64_t)font->bpp;
    uint64_t availableBits = (uint64_t)(font->glyfSize - offset) * 8;
    return lastBit <= availableBits;
}

int advanceOf(const Font *font, uint32_t codePoint, bool tabularDigits) {
    // a digit of a tabular text: the widest digit's
    bool isDigit = codePoint >= '0' && codePoint <= '9';
    if (tabularDigits && isDigit && font->digitAdvance > 0) {
        return font->digitAdvance;
    }

    // its glyph's, none when missing
    Glyph glyph;
    if (!glyphOf(font, glyphIdOf(font, codePoint), &glyph)) {
        return 0;
    }

    return glyph.advance;
}

int capHeightOf(const Font *font) {
    // the top of 'H', else of '0'
    const uint32_t references[] = { 'H', '0' };

    for (uint32_t reference : references) {
        uint32_t id = glyphIdOf(font, reference);
        Glyph glyph;
        if (id != 0 && glyphOf(font, id, &glyph)) {
            return glyph.y + glyph.height;
        }
    }

    // neither: the room above the baseline
    return font->ascent;
}

int glyphPixel(const Font *font, const Glyph *glyph, uint32_t index) {
    uint32_t position = glyph->firstBit + index * (uint32_t)font->bpp;
    return (int)readBits(glyph->bits, &position, font->bpp);
}

} // namespace draw
