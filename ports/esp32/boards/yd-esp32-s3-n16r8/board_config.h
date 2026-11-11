/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Description of the board for the shared ESP32 code (board_esp32.cpp): data
// only. VCC-GND YD-ESP32-S3, WROOM-1 N16R8 (16 MB flash, 8 MB octal PSRAM),
// host link on UART0 (COM port, CH343).
#pragma once

namespace board {

const char *const BOARD_NAME = "YD-ESP32-S3 (WROOM-1 N16R8)";   // in the identity of the device

} // namespace board
