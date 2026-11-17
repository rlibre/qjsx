/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The blocks of the thread_local variables (see tls_imxrt1062.h).
#include "tls_imxrt1062.h"
#include "memory.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

// from imxrt1062.ld: the template of a block (.tdata, then .tbss)
extern "C" char __tdata_start[];
extern "C" char __tdata_end[];
extern "C" char __tbss_end[];

namespace {

const size_t CONTROL_BLOCK = 8;   // the ABI's thread control block, before the variables

void *defaultBlock = nullptr;     // idle and timer tasks, main() before the scheduler

/**
 * Bytes of a block.
 */
size_t blockSize() {
    return CONTROL_BLOCK + (size_t)(__tbss_end - __tdata_start);
}

} // namespace

namespace tls {

/**
 * 1. the block, zeroed (.tbss);
 * 2. the initial values of .tdata after the control block.
 */
void *create() {
    // zeroed
    size_t size = blockSize();
    char *block = (char *)memory::allocate(size);
    memset(block, 0, size);

    // the initialized variables
    size_t initialized = (size_t)(__tdata_end - __tdata_start);
    memcpy(block + CONTROL_BLOCK, __tdata_start, initialized);
    return block;
}

/**
 * Frees the block.
 */
void release(void *block) {
    memory::release(block);
}

} // namespace tls

/**
 * The block of the calling task: its own (set by platform::startThread), or
 * the default one, made on the first call. Called by __aeabi_read_tp, which
 * keeps the registers the ABI requires.
 */
extern "C" void *qjsxThreadPointer(void) {
    // the task's own block
    bool schedulerStarted = xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
    if (schedulerStarted) {
        void *own = pvTaskGetThreadLocalStoragePointer(nullptr, tls::TLS_INDEX);
        if (own != nullptr) {
            return own;
        }
    }

    // the default one, once it exists
    if (defaultBlock != nullptr) {
        return defaultBlock;
    }

    // else made once, the scheduler held: two tasks without a block of their
    // own (idle, timer) cannot both make it; before the scheduler starts, only
    // main() runs (memory::allocate's malloc lock suspends it again: nested)
    if (schedulerStarted) {
        vTaskSuspendAll();
    }

    if (defaultBlock == nullptr) {
        defaultBlock = tls::create();
    }

    if (schedulerStarted) {
        xTaskResumeAll();
    }

    return defaultBlock;
}
