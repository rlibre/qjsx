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
// Changes of watched pins: an interrupt on both edges hands the pin to the
// watch shared by the FreeRTOS ports (ports/freertos/gpio_watch.h), which
// debounces and reports the changes from its event task.
#include "hal_gpio.h"
#include "gpio_watch.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"

namespace hal {
namespace gpio {

namespace {

bool serviceInstalled = false;   // the interrupt service of the GPIOs (JS thread)

/**
 * Interrupt of a watched pin: hands its number to the shared watch.
 */
void IRAM_ATTR onInterrupt(void *argument) {
    gpiowatch::postFromInterrupt((int)(intptr_t)argument);
}

/**
 * Starts, once, the interrupt service of the GPIOs and the shared watch.
 * False if one of them cannot start.
 */
bool startEvents() {
    // the interrupt service (already installed by someone else: fine)
    if (!serviceInstalled) {
        esp_err_t installed = gpio_install_isr_service(0);
        if (installed != ESP_OK && installed != ESP_ERR_INVALID_STATE) {
            return false;
        }

        serviceInstalled = true;
    }

    // the queue and the event task
    return gpiowatch::start(GPIO_NUM_MAX, &read);
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
 * Interrupt on both edges, changes reported by the shared watch.
 */
bool watch(int pin, int debounceMs, ChangeHandler *handler) {
    if (!startEvents()) {
        return false;
    }

    // record the watch (or change its debounce)
    bool added = gpiowatch::add(pin, debounceMs, handler);
    if (!added) {
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
    if (!serviceInstalled) {
        return;
    }

    gpio_intr_disable((gpio_num_t)pin);
    gpio_isr_handler_remove((gpio_num_t)pin);
    gpiowatch::remove(pin);
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
