/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Description of the board for the shared RP2350 code (board_rp2350.cpp): data
// only. Olimex PICO2-XXL: RP2350B, 16 MB QSPI flash, 8 MB QSPI PSRAM on chip
// select 1 (GPIO8), one USB-C port (the host link, USB CDC).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace board {

const char *const BOARD_NAME = "Olimex PICO2-XXL";   // in the identity of the device

// Map of the flash (16 MB): the firmware from the start (checked at boot:
// it must end before the packages), the two package slots in the upper half.
// 4 to 8 MB: free (second firmware image for an update, data).
const uint32_t PACKAGES_OFFSET = 8 * 1024 * 1024;   // bytes from the start of the flash
const uint32_t PACKAGES_SIZE = 8 * 1024 * 1024;     // two slots of half

// The PSRAM (QSPI, on QMI chip select 1): the JS heap. PSRAM_SIZE 0: none.
const int      PSRAM_CS_PIN = 8;                    // GPIO of its chip select
const size_t   PSRAM_SIZE = 8 * 1024 * 1024;        // bytes
const uint32_t PSRAM_MAX_CLOCK_HZ = 133000000;      // fastest clock of the chip (APS6404L)

} // namespace board
