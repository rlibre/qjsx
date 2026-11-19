/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The ILI9881C, a controller of MIPI-DSI panels up to 800 x 1280 (2026-11-18,
// first for the M5Stack Tab5): its set-up, as commands sent on the command
// channel of the link (hal_dsi.h). Portable: nothing of a chip here; the
// board resets the controller before (its reset pin is the board's) and
// starts the video after.
//
// A panel made with this controller needs its own registers (power,
// timings of its glass, gamma): a table per panel, given to start(). The
// tables known are in ili9881c.cpp.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace drivers {
namespace ili9881c {

// One command of a table: its parameters (at most 3).
struct Command {
    uint8_t command;               // the command (a register of the current page)
    uint8_t size;                  // parameter bytes sent
    uint8_t data[3];               // the parameters
};

// The set-up of one panel.
struct Panel {
    const Command *commands;       // its table, sent first
    size_t         count;          // commands of the table
};

// The panel of the M5Stack Tab5 (its first version, 720 x 1280, 2 lanes).
extern const Panel M5STACK_TAB5;

// The Waveshare 7-DSI-TOUCH-A (7", 720 x 1280, 2 lanes).
extern const Panel WAVESHARE_7_DSI_TOUCH_A;

// Sends a command with its parameters on the link; false if it failed.
typedef bool SendCommand(uint8_t command, const uint8_t *data, size_t size);

/**
 * Sets up a controller just reset: the table of 'panel', then 16 bits per
 * pixel, out of sleep, the display on. Blocks about 150 ms (the waits of
 * the controller). False when a command could not be sent.
 */
bool start(const Panel *panel, SendCommand *send);

} // namespace ili9881c
} // namespace drivers
