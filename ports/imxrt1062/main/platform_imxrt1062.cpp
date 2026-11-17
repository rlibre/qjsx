/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Part of platform.h specific to the i.MX RT1062 (FreeRTOS, one core):
// clock, fatal stop, tasks, random bytes. Locks, signal and sleep are shared
// with the other FreeRTOS ports (ports/freertos/platform_freertos.cpp). Also
// the FreeRTOS failure hooks.
#include "platform.h"
#include "platform_imxrt1062.h"
#include "usb_serial_imxrt1062.h"
#include "tls_imxrt1062.h"
#include "memory.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "fsl_pit.h"
#include "fsl_trng.h"

namespace platform {

namespace {

// What a task gets when ThreadOptions leaves the choice to the platform.
const size_t DEFAULT_TASK_STACK = 4096;         // bytes, as on the other ports
const UBaseType_t DEFAULT_TASK_PRIORITY = 5;    // above the idle task
const uint32_t FATAL_FLUSH_MS = 100;            // fatal() lets the USB send its message that long

// The PIT counts PERCLK (the 24 MHz crystal, clock_imxrt1062.c), down.
const uint64_t PIT_TICKS_PER_MS = 24000;

// What startThread() gives to the new task (allocated, freed by the task).
struct ThreadStart {
    ThreadFunction *run;        // thread function
    void           *argument;   // passed to run
};

/**
 * Entry of the tasks made by startThread(): gives the task its own
 * thread_local variables, runs the thread function, then deletes the task
 * (a FreeRTOS task must never return).
 */
void taskEntry(void *parameter) {
    // the start record belongs to this task: copy it, free it
    ThreadStart start = *(ThreadStart *)parameter;
    memory::release(parameter);

    // its thread_local variables
    void *variables = tls::create();
    vTaskSetThreadLocalStoragePointer(nullptr, tls::TLS_INDEX, variables);

    // run the thread function, then end the task
    start.run(start.argument);
    vTaskSetThreadLocalStoragePointer(nullptr, tls::TLS_INDEX, nullptr);
    tls::release(variables);
    vTaskDelete(nullptr);
}

// The TRNG, set up on its first use; one caller at a time.
bool  trngStarted = false;   // TRNG_Init done (under trngLock)
Mutex trngLock;              // held by randomBytes() for a whole call

} // namespace

/**
 * Channels 0 and 1 of the PIT, chained, both counting down from their
 * largest value: together a 64-bit counter at 24 MHz (never wraps).
 */
void startClock() {
    // the module
    pit_config_t config;
    PIT_GetDefaultConfig(&config);
    PIT_Init(PIT, &config);

    // channel 1 counts the turns of channel 0
    PIT_SetTimerPeriod(PIT, kPIT_Chnl_0, 0xFFFFFFFFu);
    PIT_SetTimerPeriod(PIT, kPIT_Chnl_1, 0xFFFFFFFFu);
    PIT_SetTimerChainMode(PIT, kPIT_Chnl_1, true);

    // channel 1 first, so that it sees channel 0's first turn
    PIT_StartTimer(PIT, kPIT_Chnl_1);
    PIT_StartTimer(PIT, kPIT_Chnl_0);
}

/**
 * Milliseconds since startClock(): the lifetime counter counts down from all
 * ones, so its complement is the elapsed ticks.
 */
int64_t monotonicMs() {
    uint64_t elapsed = ~PIT_GetLifetimeTimerCount(PIT);
    return (int64_t)(elapsed / PIT_TICKS_PER_MS);
}

/**
 * Prints the message, lets the USB send it, then restarts the chip.
 */
void fatal(const char *message) {
    // the message, pushed to the USB
    fprintf(stderr, "qjsx: fatal: %s\n", message);
    fflush(stderr);
    usbserial::flush(FATAL_FLUSH_MS);

    // a reset of the whole chip
    NVIC_SystemReset();
}

/**
 * Starts a FreeRTOS task running run(argument) (one core: options.core is
 * ignored). False if it cannot start.
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

    // create the task; nobody waits for it
    BaseType_t created = xTaskCreate(&taskEntry, options.name, stackWords, start, priority, nullptr);
    if (created != pdPASS) {
        memory::release(start);
        return false;
    }

    return true;
}

/**
 * Random bytes from the chip's true random number generator (TRNG), set up
 * on the first call; one caller at a time.
 */
void randomBytes(void *buffer, size_t size) {
    // one caller at a time
    trngLock.lock();

    // the generator, once
    if (!trngStarted) {
        trng_config_t config;
        TRNG_GetDefaultConfig(&config);
        if (TRNG_Init(TRNG, &config) != kStatus_Success) {
            fatal("the random number generator does not start");
        }

        trngStarted = true;
    }

    // the bytes
    if (TRNG_GetRandomData(TRNG, buffer, size) != kStatus_Success) {
        fatal("the random number generator failed");
    }

    trngLock.unlock();
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
