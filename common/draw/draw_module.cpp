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
//     kind 5: an image, its top left pixel at (x0, y0); x1 = image index
//             (0..15); y1 and pixel unused
//     pixel: the 16 bits of a pixel (5 of red, 6 of green, 5 of blue)
//
//   import { show, fontMetrics, textWidth, imageSize } from "sys/draw";
//   await show(device, items, count, x, y, width, height, background, bandRows, fonts, images);
//     device      the SpiDevice of the display (a controller that takes the
//                 MIPI DCS commands 0x2A, 0x2B, 0x2C: show() sends them)
//     items       the Int16Array; count: how many of its items, drawn in order
//     x, y, width, height   the area shown, in the coordinates of the commands
//                 and of the controller's memory (as its rotation turns it)
//     background  the pixel of what no command covers
//     bandRows    rows rendered and sent at a time
//     fonts       the font files the texts use, by index: Uint8Arrays (LVGL's
//                 binary format, uncompressed: font.h), which the caller keeps
//                 alive until the promise settles
//     images      the images the commands draw, by index: Uint8Arrays (image
//                 files made by 'qjsx build', imported with { type: "image" }:
//                 image.h), kept alive by the caller the same way
//   fontMetrics(font) -> { size, ascent, descent, lineHeight, capHeight }
//                 (pixels; capHeight: the capitals above the baseline);
//                 throws a TypeError when it is not a font this reader takes
//   textWidth(font, text, flags) -> its width in pixels
//   imageSize(image) -> { width, height } (pixels); throws a TypeError when
//                 it is not an image file of this renderer
//   screenSize() -> { width, height } of the display built into the board
//                 (board_screen.h: the M5Stack Tab5's), null without one
//   new Screen()  that display (ENODEV without one); its show(items, count,
//                 x, y, width, height, background, bandRows, fonts, images,
//                 rotation) draws the area into its image, turned, on a
//                 render task: a promise settled once the screen shows it
//
// show() copies the items (the program may write the next image at once)
// and queues ONE transaction on the device: the area told to the controller
// (columns, rows, memory write), then width x height pixels: the task of
// the SPI bus renders each band into a small buffer just before it goes out,
// so the JS thread is free meanwhile and no memory holds the whole image.
// The fonts and the images are NOT copied: read in place (in flash, for a
// file imported from the package). Its promise is resolved when the last
// band is sent.
//
// Measured on the WT32-SC01 (2026-11-17, 480 x 320, 145 commands): 13.5
// images a second when the next image is listed during the send.
#include "draw_module.h"

#include <string.h>

#include "draw.h"
#include "jsbind.h"
#include "loop.h"
#include "memory.h"
#include "platform.h"
#include "spi_module.h"

#include <errno.h>

#include "board_screen.h"

namespace {

// One frame shown: what the maker of the pixels needs, followed in the same
// block by its fonts, then its images, then its copy of the items
// (memory::allocate; freed with the transaction).
struct Frame {
    int      count;              // items
    int      fontCount;          // fonts
    int      imageCount;         // images
    int      x;                  // the area shown: its first pixel...
    int      y;
    int      width;              // ...its width, pixels...
    int      height;             // ...and its height
    uint16_t background;         // the pixel of what no command covers
};

/**
 * 'size' rounded up to a multiple of 8 (what follows it stays aligned).
 */
size_t aligned(size_t size) {
    return (size + 7) & ~(size_t)7;
}

/**
 * The fonts that follow a Frame in its block.
 */
draw::Font *fontsOf(Frame *frame) {
    return (draw::Font *)((uint8_t *)frame + aligned(sizeof(Frame)));
}

/**
 * The images that follow the fonts of a Frame.
 */
draw::Image *imagesOf(Frame *frame) {
    uint8_t *fonts = (uint8_t *)fontsOf(frame);
    return (draw::Image *)(fonts + sizeof(draw::Font) * (size_t)frame->fontCount);
}

/**
 * The items that follow the images of a Frame.
 */
int16_t *itemsOf(Frame *frame) {
    uint8_t *images = (uint8_t *)imagesOf(frame);
    return (int16_t *)(images + sizeof(draw::Image) * (size_t)frame->imageCount);
}

/**
 * How many items the array 'list' has (0 when it is not an array: none
 * given). Its length is read as a number of up to 64 bits.
 */
int64_t lengthOf(JSContext *ctx, JSValueConst list) {
    int64_t length = 0;
    if (JS_IsArray(ctx, list)) {
        JSValue value = JS_GetPropertyStr(ctx, list, "length");
        JS_ToInt64(ctx, &length, value);
        JS_FreeValue(ctx, value);
    }

    return length;
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
 * Reads the image file 'value' (a Uint8Array) into *out. False with the
 * error recorded: not bytes (TypeError), not an image of this renderer
 * (TypeError). The bytes are borrowed: the caller keeps 'value' alive while
 * *out is used.
 */
bool imageOf(js::Value value, draw::Image *out) {
    // its bytes, borrowed (the array keeps them)
    JSContext *ctx = js::context();
    uint8_t *bytes = nullptr;
    size_t size = 0;
    JSValue buffer = JS_UNDEFINED;
    if (!js::detail::bytesOf(ctx, value.get(), &bytes, &size, &buffer)) {
        js::fail<js::TypeError>("draw: an image must be a Uint8Array");
        return false;
    }

    JS_FreeValue(ctx, buffer);

    // an image file of this renderer
    if (!draw::openImage(bytes, size, out)) {
        js::fail<js::TypeError>("draw: not an image of this renderer (import a PNG file with { type: \"image\" })");
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
    Frame *frame = (Frame *)opaque;
    size_t rowBytes = (size_t)frame->width * 2;
    int firstRow = (int)(offset / rowBytes);
    int rows = (int)(size / rowBytes);

    // the background, then the commands over it
    draw::Target target = { bytes, frame->width, rows, frame->x, frame->y + firstRow };
    draw::fillRect(&target, target.originX, target.originY, frame->width, rows, frame->background);
    draw::render(itemsOf(frame), frame->count, fontsOf(frame), frame->fontCount, imagesOf(frame), frame->imageCount,
                 &target);
}

/**
 * The frame of a show (memory::allocate): the area, its background, its
 * fonts and images read, a copy of its items. nullptr with the error
 * recorded: a TypeError (items not a typed array, a font or an image that is
 * not one) or a RangeError (an empty area, bandRows below 1, too many fonts
 * or images).
 */
Frame *makeFrame(js::Value items, int count, int x, int y, int width, int height, int background, int bandRows,
                 js::Value fonts, js::Value images) {
    JSContext *ctx = js::context();

    // an area and bands of at least one pixel
    bool isSized = width >= 1 && height >= 1 && bandRows >= 1;
    if (!isSized) {
        js::fail<js::RangeError>("draw: width, height and bandRows must be at least 1");
        return nullptr;
    }

    // the fonts and the images: arrays (none: undefined), at most
    // draw::MAX_FONTS and draw::MAX_IMAGES
    int64_t fontCount = lengthOf(ctx, fonts.get());
    if (fontCount > draw::MAX_FONTS) {
        js::fail<js::RangeError>("draw: an image uses at most %d fonts", draw::MAX_FONTS);
        return nullptr;
    }

    int64_t imageCount = lengthOf(ctx, images.get());
    if (imageCount > draw::MAX_IMAGES) {
        js::fail<js::RangeError>("draw: a frame uses at most %d images", draw::MAX_IMAGES);
        return nullptr;
    }

    // the items: as many as asked, within the array
    uint8_t *itemBytes = nullptr;
    size_t itemSize = 0;
    JSValue itemBuffer = JS_UNDEFINED;
    if (!js::detail::bytesOf(ctx, items.get(), &itemBytes, &itemSize, &itemBuffer)) {
        js::fail<js::TypeError>("draw: the items must be a typed array (an Int16Array)");
        return nullptr;
    }

    JS_FreeValue(ctx, itemBuffer);
    int held = (int)(itemSize / sizeof(int16_t));
    int asked = count < 0 ? 0 : count;
    int copied = asked < held ? asked : held;

    // the frame block: header, fonts, images, items
    size_t fontBytes = sizeof(draw::Font) * (size_t)fontCount;
    size_t imageBytes = sizeof(draw::Image) * (size_t)imageCount;
    size_t blockSize = aligned(sizeof(Frame)) + fontBytes + imageBytes + sizeof(int16_t) * (size_t)copied;
    Frame *frame = (Frame *)memory::allocate(blockSize);
    *frame = Frame{ copied, (int)fontCount, (int)imageCount, x, y, width, height, (uint16_t)background };

    // each font read (in place: only where its tables are is kept)
    for (int64_t i = 0; i < fontCount; i++) {
        js::Value font(ctx, JS_GetPropertyUint32(ctx, fonts.get(), (uint32_t)i));
        if (!fontOf(font, &fontsOf(frame)[i])) {
            memory::release(frame);
            return nullptr;
        }
    }

    // each image read (in place too: its header only)
    for (int64_t i = 0; i < imageCount; i++) {
        js::Value image(ctx, JS_GetPropertyUint32(ctx, images.get(), (uint32_t)i));
        if (!imageOf(image, &imagesOf(frame)[i])) {
            memory::release(frame);
            return nullptr;
        }
    }

    memcpy(itemsOf(frame), itemBytes, sizeof(int16_t) * (size_t)copied);
    return frame;
}

// The commands that tell the controller where the pixels go (MIPI DCS, the
// same for every controller of stdlib/display): its columns, its rows (first
// and last pixel, high byte first), then the write into its memory.
const uint8_t COLUMN_RANGE = 0x2A;
const uint8_t ROW_RANGE = 0x2B;
const uint8_t MEMORY_WRITE = 0x2C;
const size_t  WINDOW_SEGMENTS = 5;     // the segments of those commands, before the pixels

/**
 * A segment that sends one command byte (DC low); its byte from
 * memory::allocate, freed with the transaction.
 */
hal::spi::Segment commandSegment(uint8_t command) {
    uint8_t *byte = (uint8_t *)memory::allocate(1);
    byte[0] = command;

    hal::spi::Segment segment = {};
    segment.write = byte;
    segment.size = 1;
    segment.dc = 0;
    return segment;
}

/**
 * A segment that sends the range 'first' to 'last' of a command (DC high,
 * each high byte first).
 */
hal::spi::Segment rangeSegment(int first, int last) {
    uint8_t *bytes = (uint8_t *)memory::allocate(4);
    bytes[0] = (uint8_t)(first >> 8);
    bytes[1] = (uint8_t)first;
    bytes[2] = (uint8_t)(last >> 8);
    bytes[3] = (uint8_t)last;

    hal::spi::Segment segment = {};
    segment.write = bytes;
    segment.size = 4;
    segment.dc = 1;
    return segment;
}

/**
 * JS: show(device, items, count, x, y, width, height, background, bandRows,
 * fonts, images): see the top of the file. Returns the promise of the write.
 * Throws the errors of makeFrame(), and a TypeError when device is not an
 * SpiDevice.
 *
 * ONE transaction (one native call: a synchronous SPI call costs about 1 ms
 * on an ESP32, measured): the area told to the controller (columns, rows,
 * memory write; as separate calls from JS they took about half of the JS
 * time of an image, 2026-11-18), then the pixels.
 */
js::Value show(js::Value device, js::Value items, int count, int x, int y, int width, int height, int background, int bandRows, js::Value fonts,
               js::Value images) {
    // the frame
    Frame *frame = makeFrame(items, count, x, y, width, height, background, bandRows, fonts, images);
    if (frame == nullptr) {
        return js::Value();
    }

    // the area: columns, rows, then the write of the pixels
    hal::spi::Segment *segments = (hal::spi::Segment *)memory::allocate(sizeof(hal::spi::Segment) * (WINDOW_SEGMENTS + 1));
    segments[0] = commandSegment(COLUMN_RANGE);
    segments[1] = rangeSegment(x, x + width - 1);
    segments[2] = commandSegment(ROW_RANGE);
    segments[3] = rangeSegment(y, y + height - 1);
    segments[4] = commandSegment(MEMORY_WRITE);

    // one made segment: all the pixels, a band at a time, in two buffers of
    // a band (one made while the other goes out; from the C heap: DMA-capable)
    size_t rowBytes = (size_t)width * 2;
    int rowsPerPiece = bandRows < height ? bandRows : height;
    size_t pieceSize = rowBytes * (size_t)rowsPerPiece;
    hal::spi::Segment *pixels = &segments[WINDOW_SEGMENTS];
    *pixels = hal::spi::Segment{};
    pixels->size = rowBytes * (size_t)height;
    pixels->dc = 1;
    pixels->make = &makeRows;
    pixels->makeOpaque = frame;
    pixels->piece = (uint8_t *)memory::allocate(pieceSize * 2);
    pixels->pieceSize = pieceSize;

    // queued on the display's device, which takes it all
    return device::queueSpiSegments(device, segments, WINDOW_SEGMENTS + 1);
}

/**
 * The screen built into the board, started (board_screen.h); nullptr on a
 * board without one (a firmware built without QJSX_HAS_SCREEN).
 */
const board::Screen *builtinScreen() {
#if QJSX_HAS_SCREEN
    return board::screen();
#else
    return nullptr;
#endif
}

/**
 * Makes an area of the screen built into the board visible (see
 * board::flushScreen); nothing on a board without one.
 */
void flushBuiltinScreen(board::ScreenArea area) {
#if QJSX_HAS_SCREEN
    board::flushScreen(area);
#else
    (void)area;
#endif
}

/**
 * Where pixel (x, y) of the drawing turned by 'rotation' (0, 90, 180, 270
 * degrees clockwise) lies in the image of the screen ('width' x 'height',
 * as the panel scans it): its index there.
 */
size_t screenIndex(int x, int y, int rotation, int width, int height) {
    int column = x;
    int row = y;
    if (rotation == 90) {
        column = width - 1 - y;
        row = x;
    } else if (rotation == 180) {
        column = width - 1 - x;
        row = height - 1 - y;
    } else if (rotation == 270) {
        column = y;
        row = height - 1 - x;
    }

    return (size_t)row * (size_t)width + (size_t)column;
}

/**
 * Copies a band of the drawing ('band': 'width' x 'rows' pixels, high byte
 * first, its first pixel at (x, y) of the drawing) into the image of the
 * screen, turned by 'rotation', each pixel in the chip's byte order.
 */
void copyBand(const uint8_t *band, int width, int rows, int x, int y, int rotation, const board::Screen *screen) {
    for (int row = 0; row < rows; row++) {
        const uint8_t *pixel = band + (size_t)row * (size_t)width * 2;

        for (int column = 0; column < width; column++) {
            size_t index = screenIndex(x + column, y + row, rotation, screen->width, screen->height);
            screen->pixels[index] = (uint16_t)((pixel[0] << 8) | pixel[1]);
            pixel += 2;
        }
    }
}

/**
 * The area of the image of the screen that the area (x, y, width, height)
 * of the drawing turned by 'rotation' covers (see screenIndex).
 */
board::ScreenArea screenArea(int x, int y, int width, int height, int rotation, const board::Screen *screen) {
    switch (rotation) {
        case 90: {
            // drawing x -> image row, drawing y -> image column from the right
            return board::ScreenArea{ screen->width - (y + height), x, height, width, rotation };
        }
        case 180: {
            return board::ScreenArea{ screen->width - (x + width), screen->height - (y + height), width, height, rotation };
        }
        case 270: {
            // drawing y -> image column, drawing x -> image row from the bottom
            return board::ScreenArea{ y, screen->height - (x + width), height, width, rotation };
        }
        default: {
            return board::ScreenArea{ x, y, width, height, rotation };
        }
    }
}

/**
 * Draws a frame into the image of the screen ('bandRows' rows at a time:
 * rendered in a small buffer, copied into the image, turned by 'rotation'
 * and in the chip's byte order), then makes the rows changed visible. No
 * JS: on the render task.
 */
void renderFrame(Frame *frame, int bandRows, int rotation, const board::Screen *screen) {
    // the buffer of a band
    int rowsPerBand = bandRows < frame->height ? bandRows : frame->height;
    size_t rowBytes = (size_t)frame->width * 2;
    uint8_t *band = (uint8_t *)memory::allocate(rowBytes * (size_t)rowsPerBand);

    // each band: rendered, then copied into the image of the screen
    for (int row = 0; row < frame->height; row += rowsPerBand) {
        int rows = frame->height - row < rowsPerBand ? frame->height - row : rowsPerBand;
        makeRows(frame, band, (size_t)row * rowBytes, (size_t)rows * rowBytes);
        copyBand(band, frame->width, rows, frame->x, frame->y + row, rotation, screen);
    }

    memory::release(band);

    // the area changed, made visible to the video
    flushBuiltinScreen(screenArea(frame->x, frame->y, frame->width, frame->height, rotation, screen));
}

// ### The render task of the screen built into the board ######################
// One task renders the frames of the screen (the JS thread goes on), one
// frame at a time: the job below is queued by Screen.show() on the JS thread
// and taken by the task, which posts its end to the loop of the program.

// A frame handed to the render task (screenLock).
struct ScreenJob {
    Frame        *frame;           // owned by the job: the task frees it
    int           bandRows;        // rows rendered at a time
    int           rotation;        // 0, 90, 180 or 270
    rt::Mailbox  *mailbox;         // where the end is posted, retained; nullptr: nobody waits any more
    rt::SourceId  source;          // the Screen that waits
    bool          waiting;         // queued, not taken yet
    bool          running;         // taken by the task, being rendered
};

ScreenJob        screenJob = {};            // the frame queued or being rendered (screenLock)
platform::Signal screenLock;                // screenJob; notified when a job is queued and when it ends
bool             renderTaskStarted = false; // JS thread

const size_t RENDER_TASK_STACK = 8 * 1024;  // the renderer keeps its buffers on the heap
const int    RENDER_TASK_PRIORITY = 5;      // above the idle work, below the drivers' tasks

/**
 * The render task: takes each job, renders it, posts its end; forever.
 */
void renderTask(void *) {
    for (;;) {
        // the next job
        screenLock.lock();
        while (!screenJob.waiting) {
            screenLock.wait(-1);
        }

        screenJob.waiting = false;
        screenJob.running = true;
        Frame *frame = screenJob.frame;
        int bandRows = screenJob.bandRows;
        int rotation = screenJob.rotation;
        screenLock.unlock();

        // rendered, outside the lock
        renderFrame(frame, bandRows, rotation, builtinScreen());
        memory::release(frame);

        // its end, to the program if it still waits
        screenLock.lock();
        screenJob.running = false;
        if (screenJob.mailbox != nullptr) {
            screenJob.mailbox->post(screenJob.source, 0, nullptr, 0, 0);
            screenJob.mailbox->release();
            screenJob.mailbox = nullptr;
        }

        screenLock.unlock();
        screenLock.notify();
    }
}

/**
 * The screen built into the board (JS class Screen of sys/draw):
 * show() hands a frame to the render task and returns its promise.
 */
class Screen {
    JS_CLASS(Screen);

public:
    /**
     * Finalizer (no JS here): forgets the screen.
     */
    ~Screen() {
        closeScreen();
    }

    /**
     * JS constructor: new Screen(); an Error with code ENODEV when the
     * board has no built-in screen (or it did not start).
     */
    void init() {
        // a screen
        if (builtinScreen() == nullptr) {
            return (void)js::failSystem(ENODEV);
        }

        // the render task, once for the device
        if (!renderTaskStarted) {
            platform::ThreadOptions options = { "screen", RENDER_TASK_STACK, RENDER_TASK_PRIORITY, platform::ANY_CORE };
            if (!platform::startThread(&renderTask, nullptr, options)) {
                return (void)js::fail("draw: cannot start the render task of the screen");
            }
            renderTaskStarted = true;
        }

        // the ends of its frames come to this object, through the loop
        _loop = rt::Loop::current();
        _source = _loop->addSource(&onMessage, this);
    }

    /**
     * JS: show(items, count, x, y, width, height, background, bandRows,
     * fonts, images, rotation): draws the area (x, y, width, height) of the
     * drawing turned by 'rotation' into the image of the screen, on the
     * render task; a promise settled once the screen shows it. The area
     * must lie on the screen as the drawing sees it (stdlib/display cuts
     * it). Fails with the errors of makeFrame(), EBADF when closed, EBUSY
     * while a frame is being drawn, a RangeError for another rotation.
     */
    JS_EXPORT(show) js::Promise<js::Value> show(js::Value items, int count, int x, int y, int width, int height, int background,
                                                int bandRows, js::Value fonts, js::Value images, int rotation) {
        // open, free, a rotation it takes
        if (_loop == nullptr) {
            return js::failSystem(EBADF);
        }

        if (_waiter.pending()) {
            return js::failSystem(EBUSY);
        }

        bool isRotation = rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270;
        if (!isRotation) {
            return js::fail<js::RangeError>("draw: rotation must be 0, 90, 180 or 270");
        }

        // the frame
        Frame *frame = makeFrame(items, count, x, y, width, height, background, bandRows, fonts, images);
        if (frame == nullptr) {
            return js::Promise<js::Value>();
        }

        // handed to the render task, unless another Screen's frame is drawn
        screenLock.lock();
        bool isBusy = screenJob.waiting || screenJob.running;
        if (isBusy) {
            screenLock.unlock();
            memory::release(frame);
            return js::failSystem(EBUSY);
        }

        rt::Mailbox *mailbox = _loop->mailbox();
        mailbox->retain();
        screenJob = ScreenJob{ frame, bandRows, rotation, mailbox, _source, true, false };
        screenLock.unlock();
        screenLock.notify();

        // a promise settled by onMessage(), the program kept alive
        js::Promise<js::Value> promise = js::Promise<js::Value>::create();
        _waiter = promise.resolver();
        updateKeepAlive();
        return promise;
    }

    /**
     * JS: closes the object (a frame being drawn is finished first, its
     * promise rejected). The screen keeps its image.
     */
    JS_EXPORT(close) void close() {
        if (_waiter.pending()) {
            _waiter.reject("draw: the screen was closed while drawing");
        }

        closeScreen();
        updateKeepAlive();
    }

private:
    /**
     * Forgets the screen (no JS: also called by the finalizer). A frame of
     * this program still queued or being drawn is waited for: it reads its
     * fonts and images.
     */
    void closeScreen() {
        // closed already
        if (_loop == nullptr) {
            return;
        }

        // nobody to tell any more; the frame finished
        screenLock.lock();
        if (screenJob.mailbox != nullptr) {
            screenJob.mailbox->release();
            screenJob.mailbox = nullptr;
        }

        while (screenJob.waiting || screenJob.running) {
            screenLock.wait(10);
        }

        screenLock.unlock();

        // then the source
        _loop->removeSource(_source);
        _loop = nullptr;
    }

    /**
     * Keeps the object (and the program) alive while a frame is drawn.
     */
    void updateKeepAlive() {
        bool keep = _waiter.pending();
        if (keep != _kept) {
            js::keepAlive(this, keep);
            _kept = keep;
        }
    }

    /**
     * The frame is on the screen (JS thread): settles its promise.
     */
    static void onMessage(void *opaque, const rt::Message *) {
        Screen *screen = (Screen *)opaque;
        if (screen->_waiter.pending()) {
            screen->_waiter.resolve(js::Value());
        }

        screen->updateKeepAlive();
    }

    js::Resolver<js::Value> _waiter;              // promise of the frame being drawn
    rt::Loop               *_loop = nullptr;      // loop of the run; nullptr: closed
    rt::SourceId            _source = { -1, 0 };  // message source of the ends of frames
    bool                    _kept = false;        // js::keepAlive(this, true) is in force
};

// ### The touch panel read by the system #####################################
// (2026-11-18, first the STM32MP157-DK2 under Linux: its kernel reads the
// controller). stdlib/touch, type "builtin", reads it through this class.

// The most fingers a report gives here.
const int MOST_TOUCH_POINTS = 10;

// Items of a finger in the array read() fills: id, x, y.
const int TOUCH_POINT_ITEMS = 3;

/**
 * The touch panel the board reads (board::touchInfo); false without one (a
 * firmware built without QJSX_HAS_SCREEN, a board whose program reads its
 * controller itself).
 */
bool builtinTouch(board::TouchInfo *out) {
#if QJSX_HAS_SCREEN
    return board::touchInfo(out);
#else
    (void)out;
    return false;
#endif
}

/**
 * The touch panel read by the system (JS class TouchPanel of sys/draw): a
 * callback at each new report, the report read into an array (nothing made
 * at each move). One at a time.
 */
class TouchPanel {
    JS_CLASS(TouchPanel);

public:
    /**
     * Finalizer (no JS here): no more reports.
     */
    ~TouchPanel() {
        stopWatching();
    }

    /**
     * JS constructor: new TouchPanel(); an Error with code ENODEV when the
     * board reads no touch panel.
     */
    void init() {
        board::TouchInfo info = {};
        if (!builtinTouch(&info)) {
            return (void)js::failSystem(ENODEV);
        }

        maxPoints = info.maxPoints;
        rangeX = info.rangeX;
        rangeY = info.rangeY;
    }

    JS_EXPORT(maxPoints) int maxPoints = 0;      // fingers it reports at most
    JS_EXPORT(rangeX) int rangeX = 0;            // its coordinates: 0 .. range - 1, the panel at rotation 0
    JS_EXPORT(rangeY) int rangeY = 0;

    /**
     * JS: onChange(callback): callback() at each new report (then read() it),
     * the program kept alive; null: no more calls.
     */
    JS_EXPORT(onChange) void onChange(js::Value callback) {
        // none any more
        JSContext *ctx = js::context();
        if (callback.isNull() || callback.isUndefined()) {
            _callback = js::Function();
            stopWatching();
            updateKeepAlive();
            return;
        }

        // a function, then the reports
        if (!JS_IsFunction(ctx, callback.get())) {
            return (void)js::fail<js::TypeError>("draw: onChange takes a function or null");
        }

        _callback = js::Function(ctx, callback.dup());
        startWatching();
        updateKeepAlive();
    }

    /**
     * JS: read(points): the last report into 'points', an Int32Array of
     * id, x, y for each finger (as many as it holds): how many fingers, or
     * -1 when no report came since the last read.
     */
    JS_EXPORT(read) int read(js::Value points) {
        // the array, as int32 items
        JSContext *ctx = js::context();
        uint8_t *bytes = nullptr;
        size_t size = 0;
        JSValue buffer = JS_UNDEFINED;
        if (!js::detail::bytesOf(ctx, points.get(), &bytes, &size, &buffer)) {
            return js::fail<js::TypeError>("draw: read takes an Int32Array");
        }

        JS_FreeValue(ctx, buffer);
        int room = (int)(size / (sizeof(int32_t) * TOUCH_POINT_ITEMS));
        int most = room < MOST_TOUCH_POINTS ? room : MOST_TOUCH_POINTS;

        // the report, then its fingers into the array
        board::TouchPoint report[MOST_TOUCH_POINTS];
        int count = readReport(report, most);
        int32_t *items = (int32_t *)bytes;

        for (int i = 0; i < count; i++) {
            items[i * TOUCH_POINT_ITEMS] = report[i].id;
            items[i * TOUCH_POINT_ITEMS + 1] = report[i].x;
            items[i * TOUCH_POINT_ITEMS + 2] = report[i].y;
        }

        return count;
    }

    /**
     * JS: no more calls; the program no longer kept alive by it.
     */
    JS_EXPORT(close) void close() {
        _callback = js::Function();
        stopWatching();
        updateKeepAlive();
    }

private:
    /**
     * The last report of the board (see board::touchRead); -1 without one.
     */
    static int readReport(board::TouchPoint *points, int most) {
#if QJSX_HAS_SCREEN
        return board::touchRead(points, most);
#else
        (void)points;
        (void)most;
        return -1;
#endif
    }

    /**
     * The reports, to this object through the loop (JS thread), once.
     */
    void startWatching() {
        if (_loop != nullptr) {
            return;
        }

        _loop = rt::Loop::current();
        _source = _loop->addSource(&onMessage, this);
        _mailbox = _loop->mailbox();
        _mailbox->retain();
#if QJSX_HAS_SCREEN
        board::touchWatch(&onReport, this);
#endif
    }

    /**
     * No more reports (no JS: also called by the finalizer). The board's
     * watch is removed first: a call in progress is waited for.
     */
    void stopWatching() {
        if (_loop == nullptr) {
            return;
        }

#if QJSX_HAS_SCREEN
        board::touchWatch(nullptr, nullptr);
#endif
        _mailbox->release();
        _mailbox = nullptr;
        _loop->removeSource(_source);
        _loop = nullptr;
    }

    /**
     * Keeps the object (and the program) alive while a callback waits.
     */
    void updateKeepAlive() {
        bool keep = !_callback.isUndefined();
        if (keep != _kept) {
            js::keepAlive(this, keep);
            _kept = keep;
        }
    }

    /**
     * A new report (the board's thread): posted to the loop of the program.
     */
    static void onReport(void *context) {
        TouchPanel *panel = (TouchPanel *)context;
        panel->_mailbox->post(panel->_source, 0, nullptr, 0, 0);
    }

    /**
     * A new report (JS thread): the callback, through a local copy (it may
     * replace itself or close the panel).
     */
    static void onMessage(void *opaque, const rt::Message *) {
        TouchPanel *panel = (TouchPanel *)opaque;
        js::Function callback = panel->_callback;
        if (!callback.isUndefined()) {
            callback();
        }
    }

    js::Function  _callback;                // onChange callback; undefined: none
    rt::Loop     *_loop = nullptr;          // loop of the run while watching; nullptr: not watching
    rt::Mailbox  *_mailbox = nullptr;       // its mailbox, retained while watching
    rt::SourceId  _source = { -1, 0 };      // message source of the reports
    bool          _kept = false;            // js::keepAlive(this, true) is in force
};

/**
 * JS: screenSize(): the size of the screen built into the board, as its
 * panel scans it ({ width, height }, pixels); null when it did not start.
 */
js::Value screenSize() {
    // none
    JSContext *ctx = js::context();
    const board::Screen *screen = builtinScreen();
    if (screen == nullptr) {
        return js::Value(ctx, JS_NULL);
    }

    // its size
    JSValue size = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, size, "width", JS_NewInt32(ctx, screen->width));
    JS_SetPropertyStr(ctx, size, "height", JS_NewInt32(ctx, screen->height));
    return js::Value(ctx, size);
}

/**
 * JS: fontMetrics(font): its size, ascent, descent, line height and the
 * height of its capitals, in pixels. Throws a TypeError when it is not a font this reader takes.
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
    JS_SetPropertyStr(ctx, metrics, "capHeight", JS_NewInt32(ctx, draw::capHeightOf(&font)));
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

/**
 * JS: imageSize(image): its width and height, in pixels. Throws a TypeError
 * when it is not an image file of this renderer.
 */
js::Value imageSize(js::Value imageValue) {
    // read
    draw::Image image;
    if (!imageOf(imageValue, &image)) {
        return js::Value();
    }

    // as an object
    JSContext *ctx = js::context();
    JSValue size = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, size, "width", JS_NewInt32(ctx, image.width));
    JS_SetPropertyStr(ctx, size, "height", JS_NewInt32(ctx, image.height));
    return js::Value(ctx, size);
}

} // namespace

/**
 * Module "sys/draw".
 */
JS_MODULE(draw, module) {
    JS_FUNCTION(module, show);
    JS_FUNCTION(module, fontMetrics);
    JS_FUNCTION(module, textWidth);
    JS_FUNCTION(module, imageSize);
    JS_FUNCTION(module, screenSize);
    JS_ADD_CLASS(module, Screen);
    JS_ADD_CLASS(module, TouchPanel);
}
