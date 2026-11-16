/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// thread_local variables on FreeRTOS (the runtime and jsbind keep their
// per-thread state in them): each task made by platform::startThread gets
// its own block, as on the ESP32, kept in FreeRTOS's thread local storage
// pointer TLS_INDEX. __aeabi_read_tp (the compiler's way to find the block,
// tls_read_tp_imxrt1062.S) returns it; the other tasks (idle, timers) and
// main() before the scheduler share a default block.
//
// A block is laid out as the ARM ABI says (TLS variant 1): 8 bytes of thread
// control block, then a copy of .tdata, then .tbss zeroed (imxrt1062.ld).
#pragma once

namespace tls {

const int TLS_INDEX = 0;   // FreeRTOS's thread local storage pointer holding the block

/**
 * A new block for a task (allocated, initialized). Owned by the caller,
 * freed by release().
 */
void *create();

/**
 * Frees a block of create().
 */
void  release(void *block);

} // namespace tls
