/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Constants of the RP2350 boards shared by main.cpp (which creates the JS task)
// and board_rp2350.cpp (which gives QuickJS its stack limit and starts the
// link).
#pragma once

#include <stddef.h>

namespace board {

const size_t JS_TASK_STACK = 48 * 1024;   // bytes of the JS task's stack, SRAM
const size_t JS_STACK_LIMIT = 40 * 1024;  // QuickJS's limit: leaves room for the C code under JS
const int    JS_TASK_PRIORITY = 5;        // like the other application tasks
const int    JS_TASK_CORE = 1;            // core 0 keeps the USB stack and the link
const int    LINK_TASK_CORE = 0;          // reader of the USB link

/**
 * Sets up the board's PSRAM (if board_config.h has one) and the JS heap in
 * it. Called first thing in main(), on core 0, before the USB, the other
 * core and the scheduler: setting up the PSRAM stops the flash for a while.
 * Without PSRAM (or if it does not answer), the JS heap stays in SRAM.
 */
void startMemory();

} // namespace board
