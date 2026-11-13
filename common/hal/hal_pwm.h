/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// PWM outputs: what the module device/pwm needs from the hardware (the
// "hal"). An output drives a pin with a square signal: a frequency, and a
// duty cycle (the share of each period the output is active: high, or low
// when inverted). The hal chooses the resolution of the duty cycle (the
// most steps the frequency allows) and says what the hardware really does:
// the frequency and the duty cycle are rounded.
//
// Outputs may share a timer, hence a frequency (the ESP32: 4 timers for 8
// outputs; the RP2350: the two outputs of a slice): a frequency no timer can
// take is refused (EBUSY). Closed, an output stops at its idle level (0, or
// 1 when inverted): the pin stays driven at that level, its timer is freed.
//
// One implementation per port (ports/esp32/main/hal_pwm_esp32.cpp: LEDC),
// and the digital twin's (host/twin/sim/twin_pwm.cpp).
//
// Threads: every function is called on the JS thread.
#pragma once

#include <stdint.h>

namespace hal {
namespace pwm {

// What an output does now.
struct Output {
    double frequency;        // Hz, as the hardware makes it
    double duty;             // 0 to 1: a whole number of steps of the resolution
    int    bits;             // resolution of the duty cycle: 2^bits steps per period
};

// What open() gives: the handle of the output, or why it failed.
struct Opened {
    int    handle;           // >= 0 when opened; -1 when not
    int    error;            // errno when not opened: EINVAL (no output on the pin, a frequency out
                             // of reach), EBUSY (the pin is an output already, no output or no
                             // timer free for that frequency)
    Output output;           // opened: what it does
};

/**
 * Opens a PWM output on 'pin' at 'frequency' (Hz), 'duty' (0 to 1) active;
 * 'invert': active low (the pin is 1 at rest).
 */
Opened open(int pin, uint32_t frequency, double duty, bool invert);

/**
 * Stops an output opened by open(): the pin stays driven at its idle level,
 * the timer is freed when no other output uses it.
 */
void close(int handle);

/**
 * Another frequency for an output; the duty cycle asked last is kept (on the
 * resolution of the new frequency). 0, or errno: EINVAL (out of reach),
 * EBUSY (no timer free for it: the output keeps its frequency). '*out': what
 * the output does after the call.
 */
int setFrequency(int handle, uint32_t frequency, Output *out);

/**
 * Another duty cycle (0 to 1) for an output, from its next period. 0, or
 * errno (EINVAL: no such output). '*out': what the output does after the
 * call.
 */
int setDuty(int handle, double duty, Output *out);

} // namespace pwm
} // namespace hal
