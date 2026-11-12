/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The watch of changing pins, shared by the FreeRTOS ports (see
// gpio_watch.h).
#include "gpio_watch.h"
#include "platform.h"

#include <stdint.h>

// ESP-IDF puts the FreeRTOS headers in a "freertos/" folder, the pico-sdk's
// FreeRTOS-Kernel does not
#if __has_include("freertos/FreeRTOS.h")
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#else
#include "FreeRTOS.h"
#include "queue.h"
#endif

namespace gpiowatch {

namespace {

const int MAX_PINS = 64;             // pins this module can watch
const int EVENT_QUEUE_LENGTH = 64;   // changes waiting for the event task (more are dropped)

// A watched pin (the event task and the JS thread, under watchLock).
struct Watch {
    bool    active;      // the pin is watched
    int     debounceMs;  // stable time before a change is reported
    bool    pending;     // a change waits for its delay to run out
    int64_t deadlineMs;  // when it does (platform::monotonicMs)
    int     lastLevel;   // last level reported (or read when the watch began)
};

Watch watches[MAX_PINS];                            // by pin; watchLock
platform::Mutex watchLock;                          // watches, changeHandler
hal::gpio::ChangeHandler *changeHandler = nullptr;  // one for every pin; set by add()
QueueHandle_t changes = nullptr;                    // pins whose level changed, from the interrupt
int watchedPins = 0;                                // pins that may be watched; set by start()
ReadPin *readLevel = nullptr;                       // the port's read; set by start()

/**
 * Ticks to wait for the next change: until the nearest debounce deadline,
 * or forever when no delay runs.
 */
TickType_t ticksUntilNextDeadline() {
    int64_t now = platform::monotonicMs();
    int64_t nearest = INT64_MAX;

    watchLock.lock();
    for (int pin = 0; pin < watchedPins; pin++) {
        if (watches[pin].active && watches[pin].pending && watches[pin].deadlineMs < nearest) {
            nearest = watches[pin].deadlineMs;
        }
    }
    watchLock.unlock();

    // no delay: forever; a deadline passed: at once; else rounded up to a tick
    if (nearest == INT64_MAX) {
        return portMAX_DELAY;
    }

    if (nearest <= now) {
        return 0;
    }

    return pdMS_TO_TICKS(nearest - now) + 1;
}

/**
 * A change of 'pin' arrived: its debounce delay starts again.
 */
void noteChange(int pin) {
    watchLock.lock();
    Watch *watch = &watches[pin];
    if (watch->active) {
        watch->pending = true;
        watch->deadlineMs = platform::monotonicMs() + watch->debounceMs;
    }

    watchLock.unlock();
}

/**
 * Reports the pins whose delay has run out and whose level differs from the
 * last one reported. The handler is called outside the lock.
 */
void reportDueChanges() {
    for (int pin = 0; pin < watchedPins; pin++) {
        // under the lock: is the delay of this pin over, and did its level change?
        watchLock.lock();
        Watch *watch = &watches[pin];
        bool due = watch->active && watch->pending && watch->deadlineMs <= platform::monotonicMs();
        int level = 0;
        bool changed = false;
        if (due) {
            watch->pending = false;
            level = readLevel(pin);
            changed = level != watch->lastLevel;
            watch->lastLevel = level;
        }

        hal::gpio::ChangeHandler *handler = changeHandler;
        watchLock.unlock();

        // report it
        if (changed && handler != nullptr) {
            handler(pin, level);
        }
    }
}

/**
 * Event task: takes the changes queued by the interrupt and reports them
 * once their debounce delay has run out, for ever.
 */
void eventTask(void *) {
    for (;;) {
        // wait for a change, at most until the next deadline
        int pin = -1;
        if (xQueueReceive(changes, &pin, ticksUntilNextDeadline()) == pdTRUE) {
            noteChange(pin);
        }

        reportDueChanges();
    }
}

} // namespace

/**
 * The queue, then the task that empties it (once).
 */
bool start(int pinCount, ReadPin *readPin) {
    if (changes != nullptr) {
        return true;
    }

    // what the task needs before it runs
    watchedPins = pinCount < MAX_PINS ? pinCount : MAX_PINS;
    readLevel = readPin;

    // the queue, then the task
    QueueHandle_t queue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(int));
    if (queue == nullptr) {
        return false;
    }

    changes = queue;
    if (!platform::startThread(&eventTask, nullptr, platform::defaultThreadOptions("gpio-events"))) {
        vQueueDelete(queue);
        changes = nullptr;
        return false;
    }

    return true;
}

/**
 * Records the watch; the level now is the reference of the next change.
 */
bool add(int pin, int debounceMs, hal::gpio::ChangeHandler *handler) {
    watchLock.lock();
    changeHandler = handler;
    bool wasActive = watches[pin].active;
    watches[pin].debounceMs = debounceMs;
    if (!wasActive) {
        watches[pin] = Watch{ true, debounceMs, false, 0, readLevel(pin) };
    }

    watchLock.unlock();
    return !wasActive;
}

/**
 * Forgets the watch.
 */
void remove(int pin) {
    watchLock.lock();
    watches[pin].active = false;
    watches[pin].pending = false;
    watchLock.unlock();
}

/**
 * Queues the pin from an interrupt, wakes the event task if needed.
 */
void postFromInterrupt(int pin) {
    BaseType_t higherPriorityWoken = pdFALSE;
    xQueueSendFromISR(changes, &pin, &higherPriorityWoken);
    portYIELD_FROM_ISR(higherPriorityWoken);
}

} // namespace gpiowatch
