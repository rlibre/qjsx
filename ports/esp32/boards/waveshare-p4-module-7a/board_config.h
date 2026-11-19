/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Description of the board for the shared ESP32 code (board_esp32.cpp): data
// only. Waveshare ESP32-P4-Module-DEV-KIT with its 7-DSI-TOUCH-A display,
// ESP32-P4 (16 MB flash, 32 MB PSRAM), host link on the USB Serial/JTAG of
// the chip (its USB-C port marked "USB"; chosen by the console of
// sdkconfig.board).
#pragma once

#include <stddef.h>

namespace board {

const char *const BOARD_NAME = "Waveshare ESP32-P4-Module-DEV-KIT (7-DSI-TOUCH-A)";   // in the identity of the device
const size_t      JS_TASK_STACK = 48 * 1024;                // stack of the JS task, bytes: about 110 nested JS calls

} // namespace board
