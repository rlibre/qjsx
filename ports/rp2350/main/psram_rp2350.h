/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The QSPI PSRAM of the RP2350 boards, on chip select 1 of the QMI (the
// memory interface that also reads the flash): set up once at boot, then
// mapped read / write in the second XIP window, through the XIP cache.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace board {

const uintptr_t PSRAM_WINDOW = 0x11000000;           // cached XIP window of chip select 1
const uintptr_t PSRAM_UNCACHED_WINDOW = 0x15000000;  // the same, uncached (checks at boot)

/**
 * Sets up the PSRAM on 'csPin' (its function becomes XIP chip select 1):
 * QPI mode, timings for the system clock and the chip's 'maxClockHz',
 * writes enabled; then writes and reads a few words of its 'size' bytes
 * back. True if a chip answered and kept what was written.
 *
 * Talking to the chip stops the XIP (the flash): call it first thing in
 * main(), on core 0, before the other core and the scheduler start; it runs
 * from RAM with the interrupts masked.
 */
bool startPsram(int csPin, uint32_t maxClockHz, size_t size);

} // namespace board
