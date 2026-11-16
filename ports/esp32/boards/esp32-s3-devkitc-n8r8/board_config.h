/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Description of the board for the shared ESP32 code (board_esp32.cpp): data
// only. Espressif ESP32-S3-DevKitC-1, WROOM-1 N8R8 (8 MB flash, 8 MB octal
// PSRAM), host link on UART0 (the "UART" USB port).
#pragma once

#include <stddef.h>

namespace board {

const char *const BOARD_NAME = "ESP32-S3-DevKitC-1 (WROOM-1 N8R8)";   // in the identity of the device
const size_t      JS_TASK_STACK = 48 * 1024;                          // stack of the JS task, bytes: about 110 nested JS calls

} // namespace board
