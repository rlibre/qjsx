/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Digital pins on the RP2350 (hal_gpio.h), with the pico-sdk.
//
// read / write / toggle go through the SIO, the single-cycle registers of
// the cores: set, clear and toggle registers act on the bits written as 1
// (no read-modify-write), in two banks (pins 0-31, pins 32-47). toggle is the
// chip's own: one write to the toggle register.
//
// The RP2350 has no open-drain output: it is made with the direction. The
// output latch stays 0; "low" drives it (output enabled), "high" lets go
// (output disabled: the pull-up raises the line).
//
// Changes of watched pins: the bank 0 GPIO interrupt (on the core of the JS
// task, which calls watch) on both edges hands the pin to the watch shared by
// the FreeRTOS ports (ports/freertos/gpio_watch.h), which debounces and
// reports the changes from its event task.
#include "hal_gpio.h"
#include "gpio_watch.h"

#include <stdint.h>

#include "hardware/gpio.h"
#include "hardware/structs/sio.h"

namespace hal {
namespace gpio {

namespace {

const uint32_t BOTH_EDGES = GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL;   // events of a watched pin
const int      BANK_PINS = 32;                                         // pins per SIO bank

Mode modes[NUM_BANK0_GPIOS];   // mode of each configured pin (JS thread)

/**
 * GPIO interrupt of a watched pin (the pico-sdk acknowledged it): hands its
 * number to the shared watch.
 */
void onInterrupt(uint pin, uint32_t) {
    gpiowatch::postFromInterrupt((int)pin);
}

/**
 * The bit of a pin in the SIO registers of its bank.
 */
uint32_t bitOf(int pin) {
    return 1u << (pin % BANK_PINS);
}

} // namespace

/**
 * The GPIOs of the chip (30 on an RP2350A, 48 on an RP2350B).
 */
int pinCount() {
    return NUM_BANK0_GPIOS;
}

/**
 * 1. the pin to the SIO, input, output latch 0;
 * 2. its pulls;
 * 3. an output (push-pull or open drain) drives 0 from now on.
 */
bool configure(int pin, Mode mode, Pull pull) {
    if (pin < 0 || pin >= (int)NUM_BANK0_GPIOS) {
        return false;
    }

    // SIO function, input, latch 0
    gpio_init((uint)pin);
    modes[pin] = mode;

    // pulls
    gpio_set_pulls((uint)pin, pull == Pull::up, pull == Pull::down);

    // an output starts low
    if (mode != Mode::input) {
        gpio_set_dir((uint)pin, GPIO_OUT);
    }

    return true;
}

/**
 * The level on the pin (input register of its bank).
 */
int read(int pin) {
    return gpio_get((uint)pin) ? 1 : 0;
}

/**
 * Push-pull: sets or clears the output bit. Open drain: 0 drives the line
 * low, 1 lets it go.
 */
void write(int pin, int level) {
    if (modes[pin] == Mode::openDrain) {
        gpio_set_dir((uint)pin, level != 0 ? GPIO_IN : GPIO_OUT);
        return;
    }

    gpio_put((uint)pin, level != 0);
}

/**
 * Inverts what the output drives, with one write to a toggle register of
 * the SIO: the output bit (push-pull), or the output enable (open drain).
 */
void toggle(int pin) {
    uint32_t bit = bitOf(pin);
    bool openDrain = modes[pin] == Mode::openDrain;

    if (pin < BANK_PINS) {
        if (openDrain) {
            sio_hw->gpio_oe_togl = bit;
        } else {
            sio_hw->gpio_togl = bit;
        }

        return;
    }

    if (openDrain) {
        sio_hw->gpio_hi_oe_togl = bit;
    } else {
        sio_hw->gpio_hi_togl = bit;
    }
}

/**
 * Interrupt on both edges, changes reported by the shared watch.
 */
bool watch(int pin, int debounceMs, ChangeHandler *handler) {
    if (!gpiowatch::start((int)NUM_BANK0_GPIOS, &read)) {
        return false;
    }

    // record the watch (or change its debounce)
    bool added = gpiowatch::add(pin, debounceMs, handler);
    if (!added) {
        return true;
    }

    // the interrupt of the pin, on the calling core (the JS task's)
    gpio_set_irq_enabled_with_callback((uint)pin, BOTH_EDGES, true, &onInterrupt);
    return true;
}

/**
 * Disables the interrupt of the pin, then forgets the watch.
 */
void unwatch(int pin) {
    gpio_set_irq_enabled((uint)pin, BOTH_EDGES, false);
    gpiowatch::remove(pin);
}

/**
 * Not watched any more, then as at boot: no function, no pull.
 */
void reset(int pin) {
    unwatch(pin);
    gpio_disable_pulls((uint)pin);
    gpio_deinit((uint)pin);
    modes[pin] = Mode::input;
}

} // namespace gpio
} // namespace hal
