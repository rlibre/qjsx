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

namespace board {

const size_t JS_TASK_STACK = 48 * 1024;   // bytes of the JS task's stack, internal RAM
const size_t JS_STACK_LIMIT = 40 * 1024;  // QuickJS's limit: leaves room for the C code under JS
const int    JS_TASK_PRIORITY = 5;        // like the other application tasks
const int    JS_TASK_CORE = 1;            // core 0 keeps the IDF tasks (WiFi later)

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
