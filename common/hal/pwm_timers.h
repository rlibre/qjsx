/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The model of a PWM made of timers shared by its outputs, as the LEDC of
// the ESP32: written once for its hal (ports/esp32/main/hal_pwm_esp32.cpp)
// and for the digital twin that plays it (host/twin/sim/twin_pwm.cpp), so
// that the twin rounds and refuses exactly as the board.
//
//   - a timer counts 2^bits steps per period, at a clock divided by a
//     fractional divider: 8 bits of fraction, 10 of integer (1.0 to
//     1023.996);
//   - the resolution is the highest the frequency allows (the divider at
//     least 1), at most 'maxBits'; a frequency too high for 1 bit, or too
//     low for the largest divider, is out of reach;
//   - the outputs of the same frequency share a timer; another frequency
//     takes a free one.
//
// Portable, no state of its own: the tables belong to the caller.
#pragma once

#include <stdint.h>

#include "hal_pwm.h"

namespace hal {
namespace pwm {

const uint32_t DIVIDER_ONE = 256;            // the divider 1.0 (8 bits of fraction)
const uint32_t DIVIDER_LIMIT = 1024 * 256;   // the divider stays below 1024 (10 bits of integer)
const int MAX_TIMERS = 8;                    // timers a table may hold

// How a timer makes a frequency.
struct TimerSetting {
    int      bits;               // resolution: 2^bits steps per period; 0: the frequency is out of reach
    uint32_t divider;            // of the clock, in 1/256
    double   frequency;          // what the timer makes, Hz
};

// A timer of a table.
struct Timer {
    uint32_t frequency;          // Hz, as asked (users > 0)
    int      users;              // outputs on it; 0: free
};

// The timers of a PWM.
struct TimerTable {
    Timer timers[MAX_TIMERS];
    int   count;                 // timers the hardware has (at most MAX_TIMERS)
};

/**
 * How a timer fed by 'clock' (Hz) makes 'frequency' (Hz): the highest
 * resolution up to 'maxBits' whose divider is at least 1, the divider
 * rounded to the nearest 1/256; bits 0 when out of reach.
 */
inline TimerSetting timerSetting(uint32_t clock, uint32_t frequency, int maxBits) {
    TimerSetting outOfReach = { 0, 0, 0 };
    if (frequency == 0) {
        return outOfReach;
    }

    // the highest resolution: frequency * 2^bits must not pass the clock
    int bits = maxBits;
    while (bits > 0 && ((uint64_t)frequency << bits) > clock) {
        bits--;
    }
    if (bits == 0) {
        return outOfReach;
    }

    // the divider for it, rounded; too large: the frequency is too low
    uint64_t perPeriod = (uint64_t)frequency << bits;
    uint64_t divider = ((uint64_t)clock * DIVIDER_ONE + perPeriod / 2) / perPeriod;
    if (divider >= DIVIDER_LIMIT) {
        return outOfReach;
    }

    // the frequency it makes
    double made = (double)clock * DIVIDER_ONE / ((double)divider * (double)((uint64_t)1 << bits));
    return TimerSetting{ bits, (uint32_t)divider, made };
}

/**
 * A timer for a new output at 'frequency': the one that runs it already,
 * else a free one (*fresh true: to set up). -1 when none is left.
 */
inline int timerTake(TimerTable *table, uint32_t frequency, bool *fresh) {
    // one that runs it
    for (int i = 0; i < table->count; i++) {
        Timer *timer = &table->timers[i];
        if (timer->users > 0 && timer->frequency == frequency) {
            timer->users++;
            *fresh = false;
            return i;
        }
    }

    // else a free one
    for (int i = 0; i < table->count; i++) {
        Timer *timer = &table->timers[i];
        if (timer->users == 0) {
            *timer = Timer{ frequency, 1 };
            *fresh = true;
            return i;
        }
    }
    return -1;
}

/**
 * An output leaves timer 'index'; true when nobody uses it any more.
 */
inline bool timerLeave(TimerTable *table, int index) {
    if (index < 0 || index >= table->count || table->timers[index].users == 0) {
        return false;
    }

    table->timers[index].users--;
    return table->timers[index].users == 0;
}

/**
 * An output on timer 'from' goes to 'frequency': the timer that runs it
 * already, else 'from' itself when the output is alone on it, else a free
 * one (*fresh true: to set up, 'from' included). -1 when none is left: the
 * output stays on 'from', nothing changed.
 */
inline int timerMove(TimerTable *table, int from, uint32_t frequency, bool *fresh) {
    // another timer that runs it
    for (int i = 0; i < table->count; i++) {
        Timer *timer = &table->timers[i];
        if (i != from && timer->users > 0 && timer->frequency == frequency) {
            timer->users++;
            timerLeave(table, from);
            *fresh = false;
            return i;
        }
    }

    // its own timer, when alone on it
    if (table->timers[from].users == 1) {
        table->timers[from].frequency = frequency;
        *fresh = true;
        return from;
    }

    // else a free one
    for (int i = 0; i < table->count; i++) {
        Timer *timer = &table->timers[i];
        if (timer->users == 0) {
            *timer = Timer{ frequency, 1 };
            timerLeave(table, from);
            *fresh = true;
            return i;
        }
    }
    return -1;
}

/**
 * The value of a duty cycle (0 to 1) on 2^bits steps, to the nearest step:
 * 0 is always idle, 2^bits always active.
 */
inline uint32_t dutySteps(double duty, int bits) {
    double steps = (double)((uint64_t)1 << bits);
    return (uint32_t)(duty * steps + 0.5);
}

/**
 * What an output on a timer set by 'setting' does with the duty cycle
 * 'duty' asked: the frequency of the timer, the duty cycle rounded to a
 * step of its resolution.
 */
inline Output outputOn(const TimerSetting *setting, double duty) {
    double steps = (double)((uint64_t)1 << setting->bits);
    double rounded = dutySteps(duty, setting->bits) / steps;
    return Output{ setting->frequency, rounded, setting->bits };
}

} // namespace pwm
} // namespace hal
