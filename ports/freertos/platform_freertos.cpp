/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Part of platform.h common to the FreeRTOS ports (ESP32, RP2350): locks,
// the signal of the event queue, sleeping. What differs between the chips
// (clock, restart, task creation and core pinning) is in each port
// (ports/esp32/main/platform_esp32.cpp...).
#include "platform.h"
#include "memory.h"

// ESP-IDF puts the FreeRTOS headers in a "freertos/" folder, the pico-sdk's
// FreeRTOS-Kernel does not
#if __has_include("freertos/FreeRTOS.h")
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#else
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#endif

namespace platform {

namespace {

/**
 * Milliseconds as FreeRTOS ticks, rounded up (a wait is never shorter than
 * asked); < 0: forever.
 */
TickType_t toTicks(int64_t milliseconds) {
    if (milliseconds < 0) {
        return portMAX_DELAY;
    }

    // rounded up, and kept below portMAX_DELAY, which means forever
    int64_t ticks = (milliseconds * configTICK_RATE_HZ + 999) / 1000;
    int64_t longest = (int64_t)portMAX_DELAY - 1;
    return (TickType_t)(ticks < longest ? ticks : longest);
}

// How often yieldIfDue() really pauses the JS task. One tick out of this
// period is lost for the JS code (1 % at most with a 1 ms tick); the task
// watchdog only needs the idle task to run once within its timeout (5 s on
// ESP-IDF), so there is a wide margin.
const int64_t YIELD_PERIOD_MS = 100;

// Tick count at the last pause of yieldIfDue(). JS task only: no lock.
TickType_t lastYieldTick = 0;

} // namespace

/**
 * Blocks the calling task for 'milliseconds' (at least one tick if > 0).
 */
void sleepMs(int64_t milliseconds) {
    vTaskDelay(toTicks(milliseconds));
}

/**
 * Every YIELD_PERIOD_MS, blocks the JS task until the next tick: the tasks
 * below it on its core run meanwhile (the idle task, which feeds the task
 * watchdog and frees the deleted tasks). Returns at once the rest of the time.
 *
 * The period is not restarted when the JS task waits in the event loop: the
 * first call after a long wait pauses for one tick that was not needed, which
 * costs less than a second call site.
 */
void yieldIfDue() {
    // not due yet: nothing to do (unsigned subtraction: right even when the
    // tick count wraps)
    TickType_t now = xTaskGetTickCount();
    TickType_t sinceLastYield = now - lastYieldTick;
    if (sinceLastYield < toTicks(YIELD_PERIOD_MS)) {
        return;
    }

    // due: let the lower-priority tasks run until the next tick
    vTaskDelay(1);
    lastYieldTick = xTaskGetTickCount();
}

// ### Mutex ###################################################################

/**
 * A FreeRTOS recursive mutex (freed by the destructor).
 */
Mutex::Mutex() {
    _state = xSemaphoreCreateRecursiveMutex();
    if (_state == nullptr) {
        fatal("out of memory (mutex)");
    }
}

/**
 * Deletes the mutex.
 */
Mutex::~Mutex() {
    vSemaphoreDelete((SemaphoreHandle_t)_state);
}

/**
 * Takes the lock (again, if the calling task already holds it).
 */
void Mutex::lock() {
    xSemaphoreTakeRecursive((SemaphoreHandle_t)_state, portMAX_DELAY);
}

/**
 * Releases one lock() of the calling task.
 */
void Mutex::unlock() {
    xSemaphoreGiveRecursive((SemaphoreHandle_t)_state);
}

// ### Signal ##################################################################

// What Signal::_state points to. FreeRTOS has no condition variable: one
// waiter (the JS task) takes a binary semaphore that notify() gives. A notify
// between the unlock and the take of wait() leaves the semaphore given, so
// the take returns at once: no wake up is lost (an extra one is allowed).
struct SignalState {
    SemaphoreHandle_t lock;     // mutex taken by lock() / unlock()
    SemaphoreHandle_t wakeUp;   // binary semaphore given by notify()
};

/**
 * Creates the mutex and the wake-up semaphore (freed by the destructor).
 */
Signal::Signal() {
    SignalState *state = (SignalState *)memory::allocate(sizeof(SignalState));
    state->lock = xSemaphoreCreateMutex();
    state->wakeUp = xSemaphoreCreateBinary();
    if (state->lock == nullptr || state->wakeUp == nullptr) {
        fatal("out of memory (signal)");
    }

    _state = state;
}

/**
 * Deletes both semaphores and frees the state.
 */
Signal::~Signal() {
    SignalState *state = (SignalState *)_state;
    vSemaphoreDelete(state->wakeUp);
    vSemaphoreDelete(state->lock);
    memory::release(state);
}

/**
 * Takes the lock.
 */
void Signal::lock() {
    xSemaphoreTake(((SignalState *)_state)->lock, portMAX_DELAY);
}

/**
 * Releases the lock.
 */
void Signal::unlock() {
    xSemaphoreGive(((SignalState *)_state)->lock);
}

/**
 * Waits for notify() at most timeoutMs (< 0: forever), with the lock held
 * (released while waiting). Returns false on timeout.
 */
bool Signal::wait(int64_t timeoutMs) {
    SignalState *state = (SignalState *)_state;

    // release the lock, wait for the wake up, take the lock again
    xSemaphoreGive(state->lock);
    bool woken = xSemaphoreTake(state->wakeUp, toTicks(timeoutMs)) == pdTRUE;
    xSemaphoreTake(state->lock, portMAX_DELAY);
    return woken;
}

/**
 * Wakes the waiting task (gives the semaphore; several notify() before the
 * wait count as one).
 */
void Signal::notify() {
    xSemaphoreGive(((SignalState *)_state)->wakeUp);
}

} // namespace platform
