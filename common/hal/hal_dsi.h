/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A display on a MIPI-DSI link (2026-11-18, first for the M5Stack Tab5): the
// link to the panel's controller and the video stream that refreshes it.
// Two channels on the same link:
//   - commands (DCS / DBI): the set-up of the controller, by its driver
//     (common/drivers, e.g. ili9881c);
//   - video (DPI): the chip sends one image held in memory to the panel,
//     again and again, by DMA. The program draws into that image (16 bits
//     a pixel, 5 of red, 6 of green, 5 of blue, in the chip's own byte
//     order) and flushes what it wrote, so that the DMA sees it.
// One display per chip. The port implements it (ESP32-P4:
// ports/esp32/main/hal_dsi_esp32.cpp); a chip without DSI does not.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hal {
namespace dsi {

// The link and the video timings of a panel (from its board: the timings
// are those of its glass).
struct Settings {
    int lanes;                     // data lanes, 1 or 2
    int laneMbps;                  // bit rate of a lane, Mb/s
    int pixelClockMhz;             // pixel clock of the video stream, MHz
    int width;                     // pixels of a row, as the panel scans them
    int height;                    // rows
    int hsyncPulse;                // horizontal sync, pixel clocks
    int hsyncBackPorch;
    int hsyncFrontPorch;
    int vsyncPulse;                // vertical sync, rows
    int vsyncBackPorch;
    int vsyncFrontPorch;
    int phyLdoChannel;             // the chip's regulator that powers the DSI PHY; -1: none
    int phyLdoMv;                  // its voltage, mV
};

/**
 * Opens the link (powers its PHY, starts the bus) and its command channel;
 * the video does not run yet. False if the chip refuses (printed).
 */
bool open(const Settings *settings);

/**
 * Sends a command of the controller with its 'size' parameter bytes (DCS,
 * in low power mode). False if it could not be sent.
 */
bool command(uint8_t command, const uint8_t *data, size_t size);

/**
 * Starts the video stream from one image in memory (width x height pixels
 * of 16 bits, row by row, black at first) and returns it; nullptr if the
 * chip refuses (printed). Called once, after the controller is set up.
 */
uint16_t *startVideo();

/**
 * Makes what the CPU wrote into the image between 'start' and 'start' +
 * 'size' bytes visible to the DMA of the video (the image lies in cached
 * memory).
 */
void flush(const void *start, size_t size);

} // namespace dsi
} // namespace hal
