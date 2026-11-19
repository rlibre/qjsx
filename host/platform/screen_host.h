/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The display built into the board, and its touch panel, for the qjsx
// executable (board_screen.h): who plays them, chosen before any program
// runs. The twin (twin_screen.cpp: an image shown in the page), or a Linux
// board run by "qjsx agent --screen / --touch" (drm_screen.cpp: the panel
// through DRM; evdev_touch.cpp: the points of the kernel's driver). None:
// board::screen() is nullptr, board::touchInfo() false.
#pragma once

#include "board_screen.h"

namespace hostscreen {

// Makes an area of the image visible (the twin posts it to the page, DRM
// copies it to the panel). Any thread.
typedef void FlushArea(board::ScreenArea area);

/**
 * The display of the board from now: its image, and what makes an area of
 * it visible. Before any program runs.
 */
void setScreen(board::Screen screen, FlushArea *flush);

// Where the image lies on the display: the display's size (its mode), the
// image's top left on it. A touch panel covers the display, not the image.
struct ScreenFrame {
    int displayWidth;                  // pixels
    int displayHeight;
    int x;                             // the image's top left on the display
    int y;
};

/**
 * The image smaller than the display, at that place (drm_screen.cpp,
 * --screen-size). Before any program runs.
 */
void setScreenFrame(ScreenFrame frame);

/**
 * Where the image lies on the display: as set, else the image filling the
 * display (no display: all 0).
 */
ScreenFrame screenFrame();

// The touch panel of a board: what board::touchInfo, touchWatch and
// touchRead do (evdev_touch.cpp).
struct TouchDriver {
    bool (*info)(board::TouchInfo *out);
    void (*watch)(board::TouchChanged *changed, void *context);
    int  (*read)(board::TouchPoint *points, int most);
};

/**
 * The touch panel of the board from now. Before any program runs.
 */
void setTouch(const TouchDriver *driver);

// The cursor of the display, for a mouse read as a touch panel
// (drm_screen.cpp: the cursor plane of the card). Coordinates of the image,
// the tip of the arrow at (x, y).
struct CursorDriver {
    bool (*show)(int x, int y);        // shown there; false: the card has none
    void (*move)(int x, int y);        // any thread
};

/**
 * The cursor of the display from now (by openDrm). Before any program runs.
 */
void setCursor(const CursorDriver *driver);

/**
 * Shows the cursor of the display at (x, y). False when the display has none
 * (no display, or a card without a cursor).
 */
bool showCursor(int x, int y);

/**
 * Moves the cursor shown; nothing without one. Any thread.
 */
void moveCursor(int x, int y);

/**
 * Takes the display of the DRM device 'path' (/dev/dri/card0): its first
 * connected connector, an image in memory shown by it, its cursor for a
 * mouse. The image is 'width' x 'height' (the mode of that size, else
 * centered in the smallest mode holding it; 0: the preferred mode's size).
 * Linux only. False after printing why.
 */
bool openDrm(const char *path, int width, int height);

/**
 * Reads the touch panel of the input device 'path' (/dev/input/eventN) on a
 * thread of its own: a multi-touch panel, or a mouse played as a panel of
 * one finger (the left button held), with the cursor of the display (needs
 * openDrm first). Linux only. False after printing why.
 */
bool openTouch(const char *path);

} // namespace hostscreen
