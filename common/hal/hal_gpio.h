/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Digital pins: what the module device/gpio needs from the hardware (the
// "hal", hardware abstraction layer). One implementation per port
// (ports/esp32/main/hal_gpio_esp32.cpp); the desktop has none, so it has no
// device/gpio.
//
// Pins are the GPIO numbers of the chip. Nothing is checked beyond the range
// (a pin of the flash, of the PSRAM or of the console UART can be taken): the
// program is responsible (user's decision, 2026-11-11).
//
// Threads: every function is called on the JS thread; the change handler is
// called on the event task of the hal (never in an interrupt, never on the
// JS thread).
#pragma once

namespace hal {
namespace gpio {

enum class Mode {
    input,      // read only
    output,     // push-pull output, its level readable too
    openDrain,  // drives low or lets go (needs a pull-up), readable too
};

enum class Pull {
    none,
    up,
    down,
};

// Called on the event task of the hal when the stable level of a watched pin
// changes (see watch()).
typedef void ChangeHandler(int pin, int level);

/**
 * Number of GPIO numbers of the chip: the pins are 0..pinCount() - 1 (some
 * numbers may not exist on a given chip: configure() fails for them).
 */
int  pinCount();

/**
 * Sets the mode and the pull of a pin. An output starts at level 0. False if
 * the chip refuses (no such pin, input only...).
 */
bool configure(int pin, Mode mode, Pull pull);

/**
 * Level of the pin (0 or 1), outputs included.
 */
int  read(int pin);

/**
 * Sets the level of an output (0 or 1). As fast as the chip allows: called
 * in loops (no check, no lock).
 */
void write(int pin, int level);

/**
 * Inverts what an output drives (its output latch, not the level read on
 * the pin).
 */
void toggle(int pin);

/**
 * Starts reporting the changes of the level of 'pin' to 'handler' (one
 * handler for every pin). The level must stay the same for debounceMs before
 * it is reported (0: each change is reported); a level equal to the last one
 * reported is not reported again. Watching a watched pin again changes its
 * debounce. False if the interrupt of the pin cannot be set up.
 */
bool watch(int pin, int debounceMs, ChangeHandler *handler);

/**
 * Stops reporting the changes of 'pin' (nothing if not watched). A change
 * being reported may still arrive once: the handler must accept it.
 */
void unwatch(int pin);

/**
 * Puts the pin back as at boot (input, no pull, not watched).
 */
void reset(int pin);

} // namespace gpio
} // namespace hal
