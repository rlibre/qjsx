/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Digital pins on the ESP32 (hal_gpio.h), with ESP-IDF.
//
// read / write / toggle go straight to the GPIO registers (W1TS / W1TC: set
// or clear the bits written as 1, no read-modify-write): they are called in
// loops. Outputs are configured as input-output, so that their level can be
// read (read); toggle inverts the output register itself.
//
// Changes of watched pins:
//   1. an interrupt on both edges puts the pin number in a queue (the
//      interrupt does nothing else: no lock, no allocation);
//   2. the event task takes the pins from the queue; each change (re)starts
//      the debounce delay of its pin;
//   3. when a delay has run out, the task reads the level and calls the
//      change handler if it differs from the last level reported.
#include "hal_gpio.h"
#include "platform.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"

namespace hal {
namespace gpio {

namespace {

const int EVENT_QUEUE_LENGTH = 64;   // changes waiting for the event task (more are dropped)

// A watched pin (the event task and the JS thread, under watchLock).
struct Watch {
    bool    active;      // the pin is watched
    int     debounceMs;  // stable time before a change is reported
    bool    pending;     // a change waits for its delay to run out
    int64_t deadlineUs;  // when it does (esp_timer_get_time)
    int     lastLevel;   // last level reported (or read when the watch began)
};

Watch watches[GPIO_NUM_MAX];           // by pin; watchLock
platform::Mutex watchLock;             // watches, changeHandler
ChangeHandler *changeHandler = nullptr;  // one for every pin; set by watch()
QueueHandle_t changes = nullptr;       // pins whose level changed, from the interrupt
bool started = false;                  // interrupt service, queue and event task (JS thread)

/**
 * Interrupt of a watched pin: queues its number for the event task.
 */
void IRAM_ATTR onInterrupt(void *argument) {
    int pin = (int)(intptr_t)argument;
    BaseType_t higherPriorityWoken = pdFALSE;
    xQueueSendFromISR(changes, &pin, &higherPriorityWoken);
    portYIELD_FROM_ISR(higherPriorityWoken);
}

/**
 * Ticks to wait for the next change: until the nearest debounce deadline,
 * or forever when no delay runs.
 */
TickType_t ticksUntilNextDeadline() {
    int64_t now = esp_timer_get_time();
    int64_t nearest = INT64_MAX;

    watchLock.lock();
    for (int pin = 0; pin < GPIO_NUM_MAX; pin++) {
        if (watches[pin].active && watches[pin].pending && watches[pin].deadlineUs < nearest) {
            nearest = watches[pin].deadlineUs;
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
    int64_t waitMs = (nearest - now + 999) / 1000;
    return pdMS_TO_TICKS(waitMs) + 1;
}

/**
 * A change of 'pin' arrived: its debounce delay starts again.
 */
void noteChange(int pin) {
    watchLock.lock();
    Watch *watch = &watches[pin];
    if (watch->active) {
        watch->pending = true;
        watch->deadlineUs = esp_timer_get_time() + (int64_t)watch->debounceMs * 1000;
    }
    watchLock.unlock();
}

/**
 * Reports the pins whose delay has run out and whose level differs from the
 * last one reported. The handler is called outside the lock.
 */
void reportDueChanges() {
    for (int pin = 0; pin < GPIO_NUM_MAX; pin++) {
        // under the lock: is the delay of this pin over, and did its level change?
        watchLock.lock();
        Watch *watch = &watches[pin];
        bool due = watch->active && watch->pending && watch->deadlineUs <= esp_timer_get_time();
        int level = 0;
        bool changed = false;
        if (due) {
            watch->pending = false;
            level = read(pin);
            changed = level != watch->lastLevel;
            watch->lastLevel = level;
        }
        ChangeHandler *handler = changeHandler;
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

/**
 * Starts, once, the interrupt service of the GPIOs, the queue and the event
 * task. False if one of them cannot start.
 */
bool startEvents() {
    if (started) {
        return true;
    }

    // the interrupt service (already installed by someone else: fine)
    esp_err_t installed = gpio_install_isr_service(0);
    if (installed != ESP_OK && installed != ESP_ERR_INVALID_STATE) {
        return false;
    }

    // the queue, then the task that empties it
    changes = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(int));
    if (changes == nullptr) {
        return false;
    }
    if (!platform::startThread(&eventTask, nullptr, platform::defaultThreadOptions("gpio-events"))) {
        return false;
    }

    started = true;
    return true;
}

} // namespace

/**
 * The GPIO numbers of the chip.
 */
int pinCount() {
    return GPIO_NUM_MAX;
}

/**
 * gpio_config with the mode and the pull; an output starts at 0.
 */
bool configure(int pin, Mode mode, Pull pull) {
    // the pin must exist, and be able to drive for an output
    bool isOutput = mode != Mode::input;
    bool valid = isOutput ? GPIO_IS_VALID_OUTPUT_GPIO(pin) : GPIO_IS_VALID_GPIO(pin);
    if (!valid) {
        return false;
    }

    // an output starts low, before it drives the pin
    if (isOutput) {
        write(pin, 0);
    }

    // mode (outputs readable too), pulls, no interrupt yet
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << pin;
    config.mode = mode == Mode::input ? GPIO_MODE_INPUT
                : mode == Mode::output ? GPIO_MODE_INPUT_OUTPUT
                : GPIO_MODE_INPUT_OUTPUT_OD;
    config.pull_up_en = pull == Pull::up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    config.pull_down_en = pull == Pull::down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config) == ESP_OK;
}

/**
 * The input register of the pin's bank.
 */
int read(int pin) {
#if SOC_GPIO_PIN_COUNT > 32
    if (pin >= 32) {
        return (int)((REG_READ(GPIO_IN1_REG) >> (pin - 32)) & 1);
    }
#endif
    return (int)((REG_READ(GPIO_IN_REG) >> pin) & 1);
}

/**
 * Sets or clears the bit of the pin (write-1-to-set / write-1-to-clear
 * registers of its bank).
 */
void write(int pin, int level) {
#if SOC_GPIO_PIN_COUNT > 32
    if (pin >= 32) {
        uint32_t bit = 1u << (pin - 32);
        REG_WRITE(level ? GPIO_OUT1_W1TS_REG : GPIO_OUT1_W1TC_REG, bit);
        return;
    }
#endif
    uint32_t bit = 1u << pin;
    REG_WRITE(level ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, bit);
}

/**
 * Inverts what the output drives: its bit in the output register (not the
 * level read on the pin, which differs for an open drain held low by
 * something else), cleared or set through W1TC / W1TS (atomic: the other
 * pins are not touched).
 */
void toggle(int pin) {
#if SOC_GPIO_PIN_COUNT > 32
    if (pin >= 32) {
        uint32_t bit = 1u << (pin - 32);
        bool driven = (REG_READ(GPIO_OUT1_REG) & bit) != 0;
        REG_WRITE(driven ? GPIO_OUT1_W1TC_REG : GPIO_OUT1_W1TS_REG, bit);
        return;
    }
#endif
    uint32_t bit = 1u << pin;
    bool driven = (REG_READ(GPIO_OUT_REG) & bit) != 0;
    REG_WRITE(driven ? GPIO_OUT_W1TC_REG : GPIO_OUT_W1TS_REG, bit);
}

/**
 * Interrupt on both edges, changes reported by the event task.
 */
bool watch(int pin, int debounceMs, ChangeHandler *handler) {
    if (!startEvents()) {
        return false;
    }

    // record the watch (or change its debounce); the level now is the
    // reference of the next change
    watchLock.lock();
    changeHandler = handler;
    bool wasActive = watches[pin].active;
    watches[pin].debounceMs = debounceMs;
    if (!wasActive) {
        watches[pin] = Watch{ true, debounceMs, false, 0, read(pin) };
    }
    watchLock.unlock();

    if (wasActive) {
        return true;
    }

    // the interrupt of the pin, on both edges
    bool ok = gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_ANYEDGE) == ESP_OK
           && gpio_isr_handler_add((gpio_num_t)pin, &onInterrupt, (void *)(intptr_t)pin) == ESP_OK
           && gpio_intr_enable((gpio_num_t)pin) == ESP_OK;
    if (!ok) {
        unwatch(pin);
    }
    return ok;
}

/**
 * Removes the interrupt of the pin, then forgets the watch.
 */
void unwatch(int pin) {
    if (!started) {
        return;
    }

    gpio_intr_disable((gpio_num_t)pin);
    gpio_isr_handler_remove((gpio_num_t)pin);

    watchLock.lock();
    watches[pin].active = false;
    watches[pin].pending = false;
    watchLock.unlock();
}

/**
 * Not watched any more, then as at boot (gpio_reset_pin).
 */
void reset(int pin) {
    unwatch(pin);
    gpio_reset_pin((gpio_num_t)pin);
}

} // namespace gpio
} // namespace hal
