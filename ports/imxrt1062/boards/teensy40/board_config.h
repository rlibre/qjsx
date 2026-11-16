/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Description of the board for the shared i.MX RT1062 code
// (board_imxrt1062.cpp): data only. PJRC Teensy 4.0: i.MX RT1062, 2 MB QSPI
// flash (Winbond W25Q16JV), no PSRAM, one USB port (the host link, USB CDC).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace board {

const char *const BOARD_NAME = "PJRC Teensy 4.0";   // in the identity of the device

// Map of the flash (2 MB from 0x60000000; design agreed 2026-11-16): the
// firmware from the start (checked at boot: it must end before the packages),
// the two package slots, room for /data (later), PJRC's last 64 KB.
const uint32_t FLASH_SIZE = 2 * 1024 * 1024;        // bytes of the chip
const uint32_t PACKAGES_OFFSET = 0x100000;          // bytes from the start of the flash
const uint32_t PACKAGES_SIZE = 512 * 1024;          // two slots of half
const uint32_t DATA_OFFSET = 0x180000;              // /data (LittleFS), not used yet
const uint32_t DATA_SIZE = 448 * 1024;              // up to PJRC's 64 KB (EEPROM emulation, restore program)

// The JS task: its stack (in the C heap, OCRAM) and what QuickJS may use of it
const size_t JS_TASK_STACK = 48 * 1024;             // bytes: about 110 nested JS calls

} // namespace board
