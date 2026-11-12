/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The watch of changing pins (hal::gpio::watch), shared by the FreeRTOS
// ports: the port keeps its registers and its interrupt, this part keeps the
// rest.
//   1. the port's interrupt of a watched pin calls postFromInterrupt(pin),
//      which only queues the pin number (no lock, no allocation);
//   2. the event task takes the pins from the queue; each change (re)starts
//      the debounce delay of its pin;
//   3. when a delay has run out, the task reads the level (through the port's
//      read function) and calls the change handler if it differs from the
//      last level reported.
// Threads: start(), add() and remove() on the JS thread; the handler on the
// event task.
#pragma once

#include "hal_gpio.h"

namespace gpiowatch {

typedef int ReadPin(int pin);   // the level of a pin, from the port

/**
 * Starts, once, the queue and the event task, for pins 0 to pinCount - 1
 * (at most 64), read by 'readPin'. False if they cannot start.
 */
bool start(int pinCount, ReadPin *readPin);

/**
 * Watches 'pin' (or changes its debounce): its level now is the reference of
 * the next change; 'handler' serves every pin. Returns true if the pin was
 * not watched yet (the port then enables its interrupt).
 */
bool add(int pin, int debounceMs, hal::gpio::ChangeHandler *handler);

/**
 * Forgets the watch of 'pin' (after the port disabled its interrupt).
 */
void remove(int pin);

/**
 * From the port's interrupt: 'pin' changed. Queues it for the event task
 * (dropped if the queue is full) and wakes the task.
 */
void postFromInterrupt(int pin);

} // namespace gpiowatch
