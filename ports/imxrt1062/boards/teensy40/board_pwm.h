/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The PWM outputs of the Teensy 4.0's pins (numbers of board_pins.h): which
// generator drives each one (a FlexPWM submodule's output A, B or X, or a
// QuadTimer channel) and the pad's mux value that connects it. From PJRC's
// teensy4/pwm.c (pwm_pin_info, MIT), checked 2026-11-16. Used by
// hal_pwm_imxrt1062.cpp.
#pragma once

#include <stdint.h>

namespace board {

// What drives a PWM pin.
enum class PwmKind : uint8_t {
    flexPwm,     // a FlexPWM submodule: one counter, so one frequency, for its outputs A, B, X
    quadTimer,   // a QuadTimer channel: its own counter
};

// The output of a FlexPWM submodule (unused for a QuadTimer).
enum class PwmOutput : uint8_t {
    x,   // output X (the third one)
    a,   // output A
    b,   // output B
};

// One PWM pin.
struct PwmPin {
    int8_t    pin;       // the board's number
    PwmKind   kind;      // what drives it
    uint8_t   module;    // FlexPWM1..4 or QuadTimer TMR1..4: 1 to 4
    uint8_t   unit;      // the submodule (FlexPWM) or the channel (QuadTimer): 0 to 3
    PwmOutput output;    // FlexPWM: A, B or X
    uint8_t   mux;       // the pad's mux value (ALT) that connects the output
};

// The pins with a PWM output, by number.
const PwmPin PWM_PINS[] = {
    { 0, PwmKind::flexPwm, 1, 1, PwmOutput::x, 4 },
    { 1, PwmKind::flexPwm, 1, 0, PwmOutput::x, 4 },
    { 2, PwmKind::flexPwm, 4, 2, PwmOutput::a, 1 },
    { 3, PwmKind::flexPwm, 4, 2, PwmOutput::b, 1 },
    { 4, PwmKind::flexPwm, 2, 0, PwmOutput::a, 1 },
    { 5, PwmKind::flexPwm, 2, 1, PwmOutput::a, 1 },
    { 6, PwmKind::flexPwm, 2, 2, PwmOutput::a, 2 },
    { 7, PwmKind::flexPwm, 1, 3, PwmOutput::b, 6 },
    { 8, PwmKind::flexPwm, 1, 3, PwmOutput::a, 6 },
    { 9, PwmKind::flexPwm, 2, 2, PwmOutput::b, 2 },
    { 10, PwmKind::quadTimer, 1, 0, PwmOutput::a, 1 },
    { 11, PwmKind::quadTimer, 1, 2, PwmOutput::a, 1 },
    { 12, PwmKind::quadTimer, 1, 1, PwmOutput::a, 1 },
    { 13, PwmKind::quadTimer, 2, 0, PwmOutput::a, 1 },
    { 14, PwmKind::quadTimer, 3, 2, PwmOutput::a, 1 },
    { 15, PwmKind::quadTimer, 3, 3, PwmOutput::a, 1 },
    { 18, PwmKind::quadTimer, 3, 1, PwmOutput::a, 1 },
    { 19, PwmKind::quadTimer, 3, 0, PwmOutput::a, 1 },
    { 22, PwmKind::flexPwm, 4, 0, PwmOutput::a, 1 },
    { 23, PwmKind::flexPwm, 4, 1, PwmOutput::a, 1 },
    { 24, PwmKind::flexPwm, 1, 2, PwmOutput::x, 4 },
    { 25, PwmKind::flexPwm, 1, 3, PwmOutput::x, 4 },
    { 28, PwmKind::flexPwm, 3, 1, PwmOutput::b, 1 },
    { 29, PwmKind::flexPwm, 3, 1, PwmOutput::a, 1 },
    { 33, PwmKind::flexPwm, 2, 0, PwmOutput::b, 1 },
    { 34, PwmKind::flexPwm, 1, 1, PwmOutput::b, 1 },
    { 35, PwmKind::flexPwm, 1, 1, PwmOutput::a, 1 },
    { 36, PwmKind::flexPwm, 1, 0, PwmOutput::b, 1 },
    { 37, PwmKind::flexPwm, 1, 0, PwmOutput::a, 1 },
    { 38, PwmKind::flexPwm, 1, 2, PwmOutput::b, 1 },
    { 39, PwmKind::flexPwm, 1, 2, PwmOutput::a, 1 },
};

const int PWM_PIN_COUNT = (int)(sizeof(PWM_PINS) / sizeof(PWM_PINS[0]));   // lines of PWM_PINS

} // namespace board
