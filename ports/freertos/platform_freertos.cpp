/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
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
    // out of memory is fatal
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

// A Signal keeps the tasks that wait on it, and notify() wakes EVERY one of
// them, each through its own task notification (FreeRTOS has no condition
// variable).
//
// Why every waiter, and why by name (2026-11-19). The first version was a
// binary semaphore given by notify(): it woke ONE task, whichever took the
// semaphore first. That holds while a Signal has a single waiter (the JS task
// on the queue of its loop), not when two kinds of tasks wait on the same
// one: in device/i2c the "i2c" task waits for a job and the JS thread waits
// for the bus to be idle. The JS thread took the wake-up meant for the task,
// saw its bus still busy and waited again; the task never ran the job: the JS
// thread blocked for good (a program that queued a read then called
// writeSync() froze within a few turns on an ESP32-P4). A wake-up cannot be
// taken by another task any more: each registered task gets its own.
//
// A task registers itself while it still holds the lock, so a notify() that
// follows a change made under the lock always finds it. A notify() with
// nobody registered wakes nobody: as a condition variable (and as this class
// on the PC), the callers change their state under the lock and check it
// again before each wait.
//
// The list of the waiting tasks is guarded by a critical section, not by a
// mutex: a few instructions on at most MAX_WAITERS entries.
//
// What it costs, measured on an ESP32-P4 (2026-11-19, temp/wsp4/sigbench,
// three runs each, against the binary semaphore):
//   - an awaited I2C read of one byte: 267 us against 270 us (281 us with a
//     mutex around the list);
//   - an awaited show() of 16 x 16 pixels: 258 us against 190 us (230 us
//     with the mutex): about 70 us more for each frame handed to the render
//     task and waited for; NOT explained (the I2C path has as many waits and
//     pays nothing);
//   - an awaited show() of 300 x 60: 2.1 ms against 2.6 ms, the whole screen
//     90 ms against 91 ms: lost in the rendering;
//   - an await of nothing, an awaited timer: unchanged (no other task).
// Memory: a FreeRTOS semaphore less for each Signal.
const int MAX_WAITERS = 4;      // tasks waiting on one Signal, at most

// The critical section of the lists of every Signal: ESP-IDF's takes a
// spinlock (two cores), the plain kernel's takes none.
#if defined(ESP_PLATFORM)
portMUX_TYPE listSpinlock = portMUX_INITIALIZER_UNLOCKED;
#define ENTER_LIST() portENTER_CRITICAL(&listSpinlock)
#define LEAVE_LIST() portEXIT_CRITICAL(&listSpinlock)
#else
#define ENTER_LIST() taskENTER_CRITICAL()
#define LEAVE_LIST() taskEXIT_CRITICAL()
#endif

// What Signal::_state points to.
struct SignalState {
    SemaphoreHandle_t lock;                  // mutex taken by lock() / unlock()
    TaskHandle_t      waiting[MAX_WAITERS];  // the tasks in wait(), not notified yet (critical section)
    int               count;                 // entries of waiting; read outside the critical section by notify() (atomic)
};

namespace {

/**
 * Takes 'task' out of the tasks waiting on a signal; false when it is not
 * among them (a notify() took it out to wake it).
 */
bool removeWaiter(SignalState *state, TaskHandle_t task) {
    ENTER_LIST();

    // its place, if it is still there
    int place = -1;
    for (int index = 0; index < state->count; index++) {
        if (state->waiting[index] == task) {
            place = index;
        }
    }

    // the last one takes its place
    bool found = place >= 0;
    if (found) {
        state->waiting[place] = state->waiting[state->count - 1];
        __atomic_store_n(&state->count, state->count - 1, __ATOMIC_RELEASE);
    }

    LEAVE_LIST();
    return found;
}

} // namespace

/**
 * Creates the mutex (freed by the destructor), nobody waiting.
 */
Signal::Signal() {
    // the state and its mutex; out of memory is fatal
    SignalState *state = (SignalState *)memory::allocate(sizeof(SignalState));
    state->lock = xSemaphoreCreateMutex();
    state->count = 0;
    bool created = state->lock != nullptr;
    if (!created) {
        fatal("out of memory (signal)");
    }

    _state = state;
}

/**
 * Deletes the mutex and frees the state.
 */
Signal::~Signal() {
    SignalState *state = (SignalState *)_state;
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
 * (released while waiting). Returns false on timeout. May return early: a
 * notification the task got from somewhere else also ends the wait.
 */
bool Signal::wait(int64_t timeoutMs) {
    SignalState *state = (SignalState *)_state;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    // registered, the lock still held: a notify() from now on finds this task
    ENTER_LIST();
    bool hasRoom = state->count < MAX_WAITERS;
    if (hasRoom) {
        state->waiting[state->count] = self;
        __atomic_store_n(&state->count, state->count + 1, __ATOMIC_RELEASE);
    }

    LEAVE_LIST();
    if (!hasRoom) {
        fatal("too many tasks wait on one signal");
    }

    // the lock released, the wake-up of this task waited for
    xSemaphoreGive(state->lock);
    bool woken = ulTaskNotifyTake(pdTRUE, toTicks(timeoutMs)) != 0;

    // out of the waiting tasks. Not there after a timeout: a notify() took
    // this task out and its wake-up is on its way: taken here, so that it does
    // not end a later wait of this task
    bool stillWaiting = removeWaiter(state, self);
    if (!woken && !stillWaiting) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        woken = true;
    }

    xSemaphoreTake(state->lock, portMAX_DELAY);
    return woken;
}

/**
 * Wakes every task waiting in wait() (lock held or not). Nobody waiting:
 * nothing to do, and nothing kept for a later wait().
 */
void Signal::notify() {
    SignalState *state = (SignalState *)_state;

    // nobody waits: the usual case of the loop's queue while the JS task works
    if (__atomic_load_n(&state->count, __ATOMIC_ACQUIRE) == 0) {
        return;
    }

    // the tasks that wait, taken out of the list
    TaskHandle_t tasks[MAX_WAITERS];
    ENTER_LIST();
    int count = state->count;
    for (int index = 0; index < count; index++) {
        tasks[index] = state->waiting[index];
    }

    __atomic_store_n(&state->count, 0, __ATOMIC_RELEASE);
    LEAVE_LIST();

    // each one woken
    for (int index = 0; index < count; index++) {
        xTaskNotifyGive(tasks[index]);
    }
}

} // namespace platform
