/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The display built into the board, for the twin (board_screen.h, see
// twin.h): an image in memory made by the part of the display, given to
// host/platform/screen_host.cpp as the display of the board. sys/draw draws
// into it on its render task, as on the board, and makes each area visible
// with board::flushScreen, which posts it to the part through the bridge; the
// part reads the pixels of the area for the page (screenPixels).
//
// Outside a twin (qjsx run, the desktop agent), or in a twin without such a
// part, there is no image: board::screen() is nullptr and sys/draw's Screen
// throws ENODEV.
//
// Threads: screenStart on the twin's thread, before any program; afterwards
// the image only changes its pixels (the render task writes them, the twin
// reads them): no lock, a pixel is 16 bits written and read whole.
#include <string.h>

#include "board_screen.h"
#include "memory.h"
#include "screen_host.h"
#include "twin.h"

namespace {

// The bridge passes a board::ScreenArea as five ints (twin_module.cpp).
static_assert(sizeof(board::ScreenArea) == 5 * sizeof(int), "board::ScreenArea must be five ints");

board::Screen image = {};          // pixels, width, height; pixels nullptr: none (yet)

/**
 * An area of the image made visible: to the part of the display, through
 * the bridge (dropped without one).
 */
void postArea(board::ScreenArea area) {
    twin::bridgePost(twin::BridgeEvent::screenFlush, 0, &area, sizeof(area));
}

} // namespace

namespace twin {

bool screenStart(int width, int height) {
    // one image for the device: the same size again is the same image
    if (image.pixels != nullptr) {
        return image.width == width && image.height == height;
    }

    // black, as a panel after its reset; the display of the board from now
    size_t bytes = (size_t)width * (size_t)height * sizeof(uint16_t);
    uint16_t *pixels = (uint16_t *)memory::allocate(bytes);
    memset(pixels, 0, bytes);
    image = board::Screen{ pixels, width, height };
    hostscreen::setScreen(image, &postArea);
    return true;
}

size_t screenPixels(int x, int y, int width, int height, uint8_t *out) {
    // none
    if (image.pixels == nullptr) {
        return 0;
    }

    // the area cut to the image
    int left = x < 0 ? 0 : x;
    int top = y < 0 ? 0 : y;
    int right = x + width > image.width ? image.width : x + width;
    int bottom = y + height > image.height ? image.height : y + height;
    if (right <= left || bottom <= top) {
        return 0;
    }

    // row by row, each pixel high byte first
    uint8_t *to = out;

    for (int row = top; row < bottom; row++) {
        const uint16_t *from = image.pixels + (size_t)row * (size_t)image.width + (size_t)left;

        for (int column = left; column < right; column++) {
            uint16_t pixel = *from++;
            *to++ = (uint8_t)(pixel >> 8);
            *to++ = (uint8_t)pixel;
        }
    }

    return (size_t)(to - out);
}

} // namespace twin
