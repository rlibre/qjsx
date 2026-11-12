/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Part of platform.h specific to the RP2350 (pico-sdk, FreeRTOS SMP): clock,
// fatal stop, tasks. Locks, signal and sleep are shared with the ESP32
// (ports/freertos/platform_freertos.cpp). Also the FreeRTOS failure hooks.
#include "platform.h"
#include "memory.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "pico/stdio.h"

namespace platform {

namespace {

// What a task gets when ThreadOptions leaves the choice to the platform.
const size_t DEFAULT_TASK_STACK = 4096;        // bytes, as on the ESP32
const UBaseType_t DEFAULT_TASK_PRIORITY = 5;   // above the idle task
const uint32_t FATAL_FLUSH_MS = 100;           // fatal() lets the USB send its message that long

// What startThread() gives to the new task (malloc'd, freed by the task).
struct ThreadStart {
    ThreadFunction *run;   // thread function
    void *argument;        // passed to run
};

/**
 * Entry of the tasks made by startThread(): runs the thread function, then
 * deletes the task (a FreeRTOS task must never return).
 */
void taskEntry(void *parameter) {
    // the start record belongs to this task: copy it, free it
    ThreadStart start = *(ThreadStart *)parameter;
    memory::release(parameter);

    // run the thread function, then end the task
    start.run(start.argument);
    vTaskDelete(nullptr);
}

} // namespace

/**
 * Milliseconds since boot (64-bit microsecond timer: never wraps).
 */
int64_t monotonicMs() {
    return (int64_t)(time_us_64() / 1000);
}

/**
 * Prints the message, lets the USB send it, then restarts the chip through
 * the watchdog.
 */
void fatal(const char *message) {
    // the message, pushed to the USB
    fprintf(stderr, "qjsx: fatal: %s\n", message);
    fflush(stderr);
    stdio_flush();

    // a busy wait: the scheduler may be what failed
    busy_wait_ms(FATAL_FLUSH_MS);
    watchdog_reboot(0, 0, 0);

    for (;;) {
        tight_loop_contents();
    }
}

/**
 * Starts a FreeRTOS task running run(argument), pinned to options.core
 * unless ANY_CORE. False if it cannot start.
 */
bool startThread(ThreadFunction *run, void *argument, ThreadOptions options) {
    // the start record is freed by the new task (or below if it cannot start)
    ThreadStart *start = (ThreadStart *)memory::allocate(sizeof(ThreadStart));
    *start = ThreadStart{ run, argument };

    // the platform's defaults where the options leave the choice; FreeRTOS
    // counts this port's stacks in words
    bool defaultStack = options.stackSize == DEFAULT_STACK;
    bool defaultPriority = options.priority == DEFAULT_PRIORITY;
    size_t stackBytes = defaultStack ? DEFAULT_TASK_STACK : options.stackSize;
    configSTACK_DEPTH_TYPE stackWords = (configSTACK_DEPTH_TYPE)(stackBytes / sizeof(StackType_t));
    UBaseType_t priority = defaultPriority ? DEFAULT_TASK_PRIORITY : (UBaseType_t)options.priority;
    UBaseType_t cores = options.core == ANY_CORE ? tskNO_AFFINITY : (UBaseType_t)(1u << options.core);

    // create the task; nobody waits for it
    BaseType_t created = xTaskCreateAffinitySet(&taskEntry, options.name, stackWords, start, priority, cores, nullptr);
    if (created != pdPASS) {
        memory::release(start);
        return false;
    }

    return true;
}

} // namespace platform

// ### FreeRTOS hooks ##########################################################

/**
 * A task went past the end of its stack (configCHECK_FOR_STACK_OVERFLOW):
 * names it, then stops.
 */
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *taskName) {
    fprintf(stderr, "qjsx: stack overflow in task \"%s\"\n", taskName);
    platform::fatal("stack overflow");
}

/**
 * FreeRTOS could not allocate a task, a queue or a semaphore.
 */
extern "C" void vApplicationMallocFailedHook(void) {
    platform::fatal("out of memory (FreeRTOS)");
}
