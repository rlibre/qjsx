/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What main.cpp and board_imxrt1062.cpp share beyond board.h: the JS task's
// settings, the start of the JS heap.
#pragma once

#include <stddef.h>

#include "board_config.h"

namespace board {

const size_t JS_NATIVE_STACK_MARGIN = 8 * 1024;                    // left to the C code under JS
const size_t JS_STACK_LIMIT = JS_TASK_STACK - JS_NATIVE_STACK_MARGIN;   // what QuickJS may use
const int    JS_TASK_PRIORITY = 5;                                 // like the other application tasks

/**
 * Sets the JS heap up over its region of the OCRAM (imxrt1062.ld). Called
 * once by main(), before the scheduler.
 */
void startMemory();

} // namespace board
