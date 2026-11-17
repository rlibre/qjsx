/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Constants of the ESP32 board shared by main.cpp (which creates the JS task)
// and board_esp32.cpp (which gives QuickJS its stack limit), and what main.cpp
// calls of the ESP32 platform beyond platform.h.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "board_config.h"

namespace board {

// The stack of the JS task is a setting of each board (JS_TASK_STACK in its
// board_config.h, internal RAM): a nested JS call takes about 366 bytes of
// it on an ESP32-S3, and what the program does not use is lost to the JS
// heap of a board without PSRAM. QuickJS throws a RangeError past its limit.
const size_t JS_NATIVE_STACK_MARGIN = 8 * 1024;                         // kept for the C code called under JS (file system, drivers)
const size_t JS_STACK_LIMIT = JS_TASK_STACK - JS_NATIVE_STACK_MARGIN;   // QuickJS's limit
const int    JS_TASK_PRIORITY = 5;                                      // like the other application tasks
const int    JS_TASK_CORE = 1;                                          // core 0 keeps the IDF tasks (WiFi later)

/**
 * Mounts the "data" partition (LittleFS) on /data, formatting it the first
 * time: the files of the programs and of the host. Called once by app_main,
 * before the JS task; false after printing why (fs then answers ENOSYS).
 */
bool mountData();

} // namespace board

namespace platform {

/**
 * Seeds the random generator at boot, before any program (platform_esp32.cpp:
 * it borrows the ADC for its entropy). Called once by app_main.
 */
void startRandom();

} // namespace platform

// What board::restart() waits for before restarting: the output still held
// by the UARTs device/serial opened, and by the USB serial functions.
namespace hal {
namespace serial {

/**
 * Waits until each open UART has sent what it holds, until 'deadlineMs'
 * (platform::monotonicMs) at most (hal_serial_esp32.cpp).
 */
void drainUntil(int64_t deadlineMs);

} // namespace serial

namespace usb {

/**
 * Pushes what each serial function holds to the host, until 'deadlineMs' at
 * most (hal_usb_esp32.cpp; only on the chips with USB OTG).
 */
void drainUntil(int64_t deadlineMs);

} // namespace usb
} // namespace hal
