// Part of platform.h specific to the ESP32 (ESP-IDF): clock, fatal stop,
// tasks. Locks, signal and sleep are shared with the RP2350
// (ports/freertos/platform_freertos.cpp).
#include "platform.h"
#include "memory.h"

#include <stdio.h>

#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace platform {

namespace {

// What a task gets when ThreadOptions leaves the choice to the platform.
const uint32_t DEFAULT_TASK_STACK = 4096;      // bytes (ESP-IDF counts stacks in bytes)
const UBaseType_t DEFAULT_TASK_PRIORITY = 5;   // above the idle task, like the IDF examples

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
    return esp_timer_get_time() / 1000;
}

/**
 * Prints the message and stops through the IDF panic handler, which prints
 * the backtrace and restarts the chip.
 */
void fatal(const char *message) {
    fprintf(stderr, "espjs: fatal: %s\n", message);
    fflush(stderr);
    esp_system_abort(message);
}

/**
 * Starts a FreeRTOS task running run(argument), pinned to options.core
 * unless ANY_CORE. False if it cannot start.
 */
bool startThread(ThreadFunction *run, void *argument, ThreadOptions options) {
    // the start record is freed by the new task (or below if it cannot start)
    ThreadStart *start = (ThreadStart *)memory::allocate(sizeof(ThreadStart));
    *start = ThreadStart{ run, argument };

    // the platform's defaults where the options leave the choice
    bool defaultStack = options.stackSize == DEFAULT_STACK;
    bool defaultPriority = options.priority == DEFAULT_PRIORITY;
    uint32_t stack = defaultStack ? DEFAULT_TASK_STACK : (uint32_t)options.stackSize;
    UBaseType_t priority = defaultPriority ? DEFAULT_TASK_PRIORITY : (UBaseType_t)options.priority;
    BaseType_t core = options.core == ANY_CORE ? tskNO_AFFINITY : (BaseType_t)options.core;

    // create the task; nobody waits for it
    BaseType_t created = xTaskCreatePinnedToCore(&taskEntry, options.name, stack, start, priority, nullptr, core);
    if (created != pdPASS) {
        memory::release(start);
        return false;
    }

    return true;
}

} // namespace platform
