/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "sys/draw" (user, 2026-11-17; only in a firmware built with
// QJSX_DRAW): shows a list of drawing commands on a display wired on SPI,
// rendered in native code (draw.h) a few rows at a time while they are sent.
// For stdlib/display, which programs import; not meant for them directly.
//
// The program says what to draw, nothing is drawn: it writes its commands
// itself in an Int16Array (another typed array is read as its bytes, 16 bits
// an item; no native call per command: a call from JS costs about 0.25 ms on
// an ESP32, writing a command 0.02 ms), 6 items each:
//
//   [kind, pixel, x0, y0, x1, y1]
//     kind 0: a filled rectangle, first pixel (x0, y0), x1 wide, y1 high
//     kind 1: a line from (x0, y0) to (x1, y1), both included
//     kind 2: a text, the top left of its line at (x0, y0); x1 = font index
//             (0..15) | flags << 4 (1: tabular digits); y1 = its length in UTF-16
//             units, which follow, one item each
//     kind 3: an arc, center (x0, y0), outer radius x1, thickness y1 (inward),
//             then 2 items: its start and end angles in tenths of a degree,
//             0 at the top, clockwise
//     kind 4: a rounded rectangle, first pixel (x0, y0), x1 wide, y1 high,
//             then 2 items: the radius of its corners, its thickness (0:
//             filled)
//     pixel: the 16 bits of a pixel (5 of red, 6 of green, 5 of blue)
//
//   import { show, fontMetrics, textWidth } from "sys/draw";
//   await show(device, items, count, x, y, width, height, background, bandRows, fonts);
//     device      the SpiDevice of the display, its area of pixels chosen
//                 already (the commands of its controller: the caller's)
//     items       the Int16Array; count: how many of its items, drawn in order
//     x, y, width, height   the area shown, in the coordinates of the commands
//     background  the pixel of what no command covers
//     bandRows    rows rendered and sent at a time
//     fonts       the font files the texts use, by index: Uint8Arrays (LVGL's
//                 binary format, uncompressed: font.h), which the caller keeps
//                 alive until the promise settles
//   fontMetrics(font) -> { size, ascent, descent, lineHeight } (pixels);
//                 throws a TypeError when it is not a font this reader takes
//   textWidth(font, text, flags) -> its width in pixels
//
// show() copies the items (the program may write the next image at once)
// and queues one write of width x height pixels on the device: the task of
// the SPI bus renders each band into a small buffer just before it goes out,
// so the JS thread is free meanwhile and no memory holds the whole image.
// The fonts are NOT copied: read in place (in flash, for a font imported as
// bytes from the package). Its promise is resolved when the last band is
// sent.
//
// Measured on the WT32-SC01 (2026-11-17, 480 x 320, 145 commands): 13.5
// images a second when the next image is listed during the send.
#include "draw_module.h"

#include <string.h>

#include "draw.h"
#include "jsbind.h"
#include "memory.h"
#include "spi_module.h"

namespace {

// What the maker of the pixels needs, followed in the same block by its
// fonts, then by its copy of the items (memory::allocate; freed with the
// transaction).
struct Image {
    int      count;              // items
    int      fontCount;          // fonts
    int      x;                  // the area shown: its first pixel...
    int      y;
    int      width;              // ...and its width, pixels
    uint16_t background;         // the pixel of what no command covers
};

/**
 * 'size' rounded up to a multiple of 8 (what follows it stays aligned).
 */
size_t aligned(size_t size) {
    return (size + 7) & ~(size_t)7;
}

/**
 * The fonts that follow an Image in its block.
 */
draw::Font *fontsOf(Image *image) {
    return (draw::Font *)((uint8_t *)image + aligned(sizeof(Image)));
}

/**
 * The items that follow the fonts of an Image.
 */
int16_t *itemsOf(Image *image) {
    uint8_t *fonts = (uint8_t *)fontsOf(image);
    return (int16_t *)(fonts + sizeof(draw::Font) * (size_t)image->fontCount);
}

/**
 * Reads the font file 'value' (a Uint8Array) into *out. False with the error
 * recorded: not bytes (TypeError), not a font this reader takes (TypeError).
 * The bytes are borrowed: the caller keeps 'value' alive while *out is used.
 */
bool fontOf(js::Value value, draw::Font *out) {
    // its bytes, borrowed (the array keeps them)
    JSContext *ctx = js::context();
    uint8_t *bytes = nullptr;
    size_t size = 0;
    JSValue buffer = JS_UNDEFINED;
    if (!js::detail::bytesOf(ctx, value.get(), &bytes, &size, &buffer)) {
        js::fail<js::TypeError>("draw: a font must be a Uint8Array");
        return false;
    }

    JS_FreeValue(ctx, buffer);

    // a font file this reader takes
    if (!draw::openFont(bytes, size, out)) {
        js::fail<js::TypeError>("draw: not a font this renderer reads (LVGL binary font, made with --no-compress)");
        return false;
    }

    return true;
}

/**
 * Converts 'size' bytes of UTF-8 into UTF-16 code units in 'units' (room for
 * 'size' of them: never more units than bytes); returns how many. A byte
 * that starts no valid sequence is taken as itself.
 */
int utf8ToUnits(const uint8_t *bytes, size_t size, uint16_t *units) {
    int count = 0;

    for (size_t at = 0; at < size;) {
        // the length of the sequence, from its first byte
        uint32_t first = bytes[at];
        int length = first < 0x80 ? 1 : (first >> 5) == 0x6 ? 2 : (first >> 4) == 0xE ? 3 : (first >> 3) == 0x1E ? 4 : 1;
        if (at + (size_t)length > size) {
            length = 1;
        }

        // its code point: the bits of the first byte, then 6 per byte
        uint32_t codePoint = length == 1 ? first : first & (0x7F >> length);
        for (int i = 1; i < length; i++) {
            codePoint = (codePoint << 6) | (bytes[at + (size_t)i] & 0x3F);
        }

        at += (size_t)length;

        // one unit, or a surrogate pair above the first plane
        if (codePoint >= 0x10000) {
            codePoint -= 0x10000;
            units[count++] = (uint16_t)(0xD800 + (codePoint >> 10));
            units[count++] = (uint16_t)(0xDC00 + (codePoint & 0x3FF));
        } else {
            units[count++] = (uint16_t)codePoint;
        }
    }

    return count;
}

/**
 * Maker of a made segment (hal_spi.h; on the task of the bus, or the JS
 * thread for a transaction it runs itself): renders into 'bytes' the rows of
 * the image that hold its 'size' bytes at 'offset' (whole rows: a piece is
 * a band).
 */
void makeRows(void *opaque, uint8_t *bytes, size_t offset, size_t size) {
    // which rows of the area
    Image *image = (Image *)opaque;
    size_t rowBytes = (size_t)image->width * 2;
    int firstRow = (int)(offset / rowBytes);
    int rows = (int)(size / rowBytes);

    // the background, then the commands over it
    draw::Target target = { bytes, image->width, rows, image->x, image->y + firstRow };
    draw::fillRect(&target, target.originX, target.originY, image->width, rows, image->background);
    draw::render(itemsOf(image), image->count, fontsOf(image), image->fontCount, &target);
}

/**
 * JS: show(device, items, count, x, y, width, height, background, bandRows,
 * fonts): see the top of the file. Returns the promise of the write. Throws
 * a TypeError (device is not an SpiDevice, items not a typed array, a font
 * that is not one) or a RangeError (an empty area, bandRows below 1, too
 * many fonts).
 */
js::Value show(js::Value device, js::Value items, int count, int x, int y, int width, int height, int background, int bandRows, js::Value fonts) {
    JSContext *ctx = js::context();

    // an area and bands of at least one pixel
    bool isSized = width >= 1 && height >= 1 && bandRows >= 1;
    if (!isSized) {
        return js::fail<js::RangeError>("draw: width, height and bandRows must be at least 1");
    }

    // the fonts: an array (none: undefined), at most draw::MAX_FONTS
    int64_t fontCount = 0;
    bool hasFonts = JS_IsArray(ctx, fonts.get());
    if (hasFonts) {
        JSValue length = JS_GetPropertyStr(ctx, fonts.get(), "length");
        JS_ToInt64(ctx, &fontCount, length);
        JS_FreeValue(ctx, length);
    }

    if (fontCount > draw::MAX_FONTS) {
        return js::fail<js::RangeError>("draw: an image uses at most %d fonts", draw::MAX_FONTS);
    }

    // the items: as many as asked, within the array
    uint8_t *itemBytes = nullptr;
    size_t itemSize = 0;
    JSValue itemBuffer = JS_UNDEFINED;
    if (!js::detail::bytesOf(ctx, items.get(), &itemBytes, &itemSize, &itemBuffer)) {
        return js::fail<js::TypeError>("draw: the items must be a typed array (an Int16Array)");
    }

    JS_FreeValue(ctx, itemBuffer);
    int held = (int)(itemSize / sizeof(int16_t));
    int asked = count < 0 ? 0 : count;
    int copied = asked < held ? asked : held;

    // the image block: header, fonts, items
    size_t fontBytes = sizeof(draw::Font) * (size_t)fontCount;
    size_t blockSize = aligned(sizeof(Image)) + fontBytes + sizeof(int16_t) * (size_t)copied;
    Image *image = (Image *)memory::allocate(blockSize);
    *image = Image{ copied, (int)fontCount, x, y, width, (uint16_t)background };

    // each font read (in place: only where its tables are is kept)
    for (int64_t i = 0; i < fontCount; i++) {
        js::Value font(ctx, JS_GetPropertyUint32(ctx, fonts.get(), (uint32_t)i));
        if (!fontOf(font, &fontsOf(image)[i])) {
            memory::release(image);
            return js::Value();
        }
    }

    memcpy(itemsOf(image), itemBytes, sizeof(int16_t) * (size_t)copied);

    // one made segment: all the pixels, a band at a time, in two buffers of
    // a band (one made while the other goes out; from the C heap: DMA-capable)
    size_t rowBytes = (size_t)width * 2;
    int rowsPerPiece = bandRows < height ? bandRows : height;
    size_t pieceSize = rowBytes * (size_t)rowsPerPiece;
    hal::spi::Segment *segment = (hal::spi::Segment *)memory::allocate(sizeof(hal::spi::Segment));
    *segment = hal::spi::Segment{};
    segment->size = rowBytes * (size_t)height;
    segment->dc = 1;
    segment->make = &makeRows;
    segment->makeOpaque = image;
    segment->piece = (uint8_t *)memory::allocate(pieceSize * 2);
    segment->pieceSize = pieceSize;

    // queued on the display's device, which takes it all
    return device::queueSpiSegments(device, segment, 1);
}

/**
 * JS: fontMetrics(font): its size, ascent, descent and line height, in
 * pixels. Throws a TypeError when it is not a font this reader takes.
 */
js::Value fontMetrics(js::Value fontValue) {
    // read
    draw::Font font;
    if (!fontOf(fontValue, &font)) {
        return js::Value();
    }

    // as an object
    JSContext *ctx = js::context();
    JSValue metrics = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, metrics, "size", JS_NewInt32(ctx, font.pixelSize));
    JS_SetPropertyStr(ctx, metrics, "ascent", JS_NewInt32(ctx, font.ascent));
    JS_SetPropertyStr(ctx, metrics, "descent", JS_NewInt32(ctx, font.descent));
    JS_SetPropertyStr(ctx, metrics, "lineHeight", JS_NewInt32(ctx, font.ascent - font.descent));
    return js::Value(ctx, metrics);
}

/**
 * JS: textWidth(font, text, flags): the width of 'text' drawn with 'font',
 * in whole pixels. Throws a TypeError (not a font, not a string).
 */
int textWidth(js::Value fontValue, js::Value textValue, int flags) {
    // the font
    draw::Font font;
    if (!fontOf(fontValue, &font)) {
        return 0;
    }

    // the text, as UTF-16 code units
    JSContext *ctx = js::context();
    if (!JS_IsString(textValue.get())) {
        return js::fail<js::TypeError>("draw: the text must be a string");
    }

    size_t utf8Size = 0;
    const char *utf8 = JS_ToCStringLen(ctx, &utf8Size, textValue.get());
    if (utf8 == nullptr) {
        js::detail::markPendingException();
        return 0;
    }

    // at most one unit per byte
    uint16_t *units = (uint16_t *)memory::allocate(sizeof(uint16_t) * (utf8Size > 0 ? utf8Size : 1));
    int count = utf8ToUnits((const uint8_t *)utf8, utf8Size, units);
    JS_FreeCString(ctx, utf8);

    // measured
    int width = draw::textWidth(&font, units, count, flags);
    memory::release(units);
    return width;
}

} // namespace

/**
 * Module "sys/draw".
 */
JS_MODULE(draw, module) {
    JS_FUNCTION(module, show);
    JS_FUNCTION(module, fontMetrics);
    JS_FUNCTION(module, textWidth);
}
