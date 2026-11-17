/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Description of the board for the shared ESP32 code (board_esp32.cpp): data
// only. Wireless-Tag WT32-SC01 v3.2, ESP32-WROVER-B (original ESP32, 4 MB
// flash, SPI PSRAM), host link on UART0 (CP210x USB bridge).
#pragma once

#include <stddef.h>

namespace board {

const char *const BOARD_NAME = "WT32-SC01 v3.2 (ESP32-WROVER-B)";   // in the identity of the device
const size_t      JS_TASK_STACK = 48 * 1024;                        // stack of the JS task, bytes: about 110 nested JS calls
const int         DAC_FULL_SCALE_MV = 3300;                         // device/dac: the voltage of code 255, mV (VDD3P3_RTC)

} // namespace board
