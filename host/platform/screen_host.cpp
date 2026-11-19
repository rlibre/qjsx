/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// board_screen.h for the qjsx executable (see screen_host.h): the display
// and the touch panel set before any program runs, by the twin or by the
// options of "qjsx agent". Set once, then only read: no lock.
#include "screen_host.h"

namespace {

board::Screen          image = {};          // pixels nullptr: no display
hostscreen::FlushArea *flushArea = nullptr;
hostscreen::ScreenFrame frame = {};        // where the image lies on the display
bool                   framed = false;      // frame set: the image is smaller than the display
const hostscreen::TouchDriver *touch = nullptr;   // nullptr: no touch panel read by the board
const hostscreen::CursorDriver *cursor = nullptr; // nullptr: no cursor
bool                   cursorShown = false;

} // namespace

namespace hostscreen {

void setScreen(board::Screen screen, FlushArea *flush) {
    image = screen;
    flushArea = flush;
}

void setScreenFrame(ScreenFrame place) {
    frame = place;
    framed = true;
}

ScreenFrame screenFrame() {
    if (framed) {
        return frame;
    }

    return ScreenFrame{ image.width, image.height, 0, 0 };
}

void setTouch(const TouchDriver *driver) {
    touch = driver;
}

void setCursor(const CursorDriver *driver) {
    cursor = driver;
}

bool showCursor(int x, int y) {
    cursorShown = cursor != nullptr && cursor->show(x, y);
    return cursorShown;
}

void moveCursor(int x, int y) {
    if (cursorShown) {
        cursor->move(x, y);
    }
}

} // namespace hostscreen

namespace board {

bool startScreen() {
    return image.pixels != nullptr;
}

const Screen *screen() {
    return image.pixels != nullptr ? &image : nullptr;
}

void flushScreen(ScreenArea area) {
    if (flushArea != nullptr) {
        flushArea(area);
    }
}

bool touchInfo(TouchInfo *out) {
    return touch != nullptr && touch->info(out);
}

void touchWatch(TouchChanged *changed, void *context) {
    if (touch != nullptr) {
        touch->watch(changed, context);
    }
}

int touchRead(TouchPoint *points, int most) {
    return touch != nullptr ? touch->read(points, most) : -1;
}

} // namespace board
