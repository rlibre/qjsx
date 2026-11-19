/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The display built into a board (2026-11-18, first the M5Stack Tab5): a
// panel refreshed from an image in memory (MIPI-DSI video, hal_dsi.h), which
// sys/draw draws into (stdlib/display, type "builtin"). Only a board with
// such a display implements this (its firmware is built with
// QJSX_HAS_SCREEN); the others pay nothing. The twin implements it too
// (host/twin/sim/twin_screen.cpp): an image in memory shown in the page.
#pragma once

#include <stdint.h>

namespace board {

// The image of the display, as the panel scans it (its own orientation:
// stdlib/display turns the drawing).
struct Screen {
    uint16_t *pixels;              // row by row, 16 bits a pixel (5 red, 6 green, 5 blue), the chip's byte order
    int       width;               // pixels of a row
    int       height;              // rows
};

/**
 * Starts the display: resets and sets up its controller, starts the video
 * from a black image (the backlight stays as it is: the program drives it).
 * Called once, at boot, before any program. False after printing why: the
 * board then has no display.
 */
bool startScreen();

/**
 * The display, once started; nullptr if it could not start.
 */
const Screen *screen();

// An area of the image just written: in pixels of the image (as the panel
// scans it), and how the drawing that wrote it is turned.
struct ScreenArea {
    int x;                         // first column
    int y;                         // first row
    int width;                     // columns
    int height;                    // rows
    int rotation;                  // the drawing turned 0, 90, 180 or 270 degrees clockwise
};

/**
 * Makes 'area' of the image, just written, visible to the video (the rows it
 * covers, or exactly the area: the board's choice).
 */
void flushScreen(ScreenArea area);

// ### The touch panel of the display, read by the system #####################
// (2026-11-18, first the STM32MP157-DK2 under Linux: its kernel driver reads
// the controller, the board gives the points it reports). A board whose
// touch controller the program reads itself (stdlib/touch over device/i2c,
// as the Tab5's GT911) has none: touchInfo() is false.

// What the touch panel is.
struct TouchInfo {
    int maxPoints;                 // fingers it reports at most
    int rangeX;                    // its coordinates: 0 .. range - 1, the panel at rotation 0
    int rangeY;
};

// A finger of a report.
struct TouchPoint {
    int id;                        // the same while the finger stays
    int x;                         // in the range of TouchInfo
    int y;
};

// Called when a new report came (any thread: post, do not work).
typedef void TouchChanged(void *context);

/**
 * The touch panel, if the board reads one; false otherwise.
 */
bool touchInfo(TouchInfo *out);

/**
 * Calls 'changed(context)' at each new report; nullptr: no more calls.
 */
void touchWatch(TouchChanged *changed, void *context);

/**
 * The last report into 'points' (room for 'most'): how many fingers, or -1
 * when no report came since the last call. Any thread.
 */
int touchRead(TouchPoint *points, int most);

} // namespace board
