/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// hal::pwm of the digital twin (see twin.h): the PWM of the board
// description ("pwm": { outputs, timers, clock, maxBits }), rounded and
// refused as that hardware (common/hal/pwm_timers.h, shared with the hal of
// the ESP32). Any pin of the board can be an output, as through the GPIO
// matrix of the ESP32. The pins learn their signal through twin::pwmDrive
// and twin::pwmStop (twin.cpp).
//
// Threads: hal::pwm on the JS thread; pwmRead in load(), before it.
#include "hal_pwm.h"
#include "pwm_timers.h"
#include "twin.h"

#include <errno.h>
#include <stdio.h>

namespace {

const int MAX_OUTPUTS = 16;              // outputs a board may have
const int MAX_BITS = 20;                 // the widest counter a board may have

// The PWM of the board ("pwm" of the description; written by pwmRead).
int outputCount = 0;                     // "outputs"; 0: the board has no PWM
uint32_t clockHz = 0;                    // "clock", Hz
int timerBits = 0;                       // "maxBits": the widest counter of its timers

// An output.
struct Channel {
    bool   used;                         // open() gave it
    int    pin;                          // its pin
    int    timer;                        // the timer it runs on
    bool   invert;                       // active low
    double duty;                         // the duty cycle asked last, 0 to 1
};

Channel channels[MAX_OUTPUTS];                           // JS thread
hal::pwm::TimerTable timers = { {}, 0 };                 // "timers" of them (JS thread)
hal::pwm::TimerSetting settings[hal::pwm::MAX_TIMERS];   // how each timer in use makes its frequency

/**
 * Reads the number 'name' of 'object' into 'out' when it lies in [low,
 * high]; false otherwise (missing included).
 */
bool readNumber(JSContext *ctx, JSValueConst object, const char *name, double low, double high, double *out) {
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    bool valid = JS_IsNumber(value) && JS_ToFloat64(ctx, out, value) == 0 && *out >= low && *out <= high;
    JS_FreeValue(ctx, value);
    return valid;
}

/**
 * An open output by its handle; nullptr if there is none.
 */
Channel *channelOf(int handle) {
    if (handle < 0 || handle >= outputCount || !channels[handle].used) {
        return nullptr;
    }
    return &channels[handle];
}

/**
 * What an output does: the frequency of its timer, its duty cycle rounded to
 * a step.
 */
hal::pwm::Output outputOf(int handle) {
    const Channel *channel = &channels[handle];
    return hal::pwm::outputOn(&settings[channel->timer], channel->duty);
}

/**
 * Tells the pin of an output what it does now.
 */
void showOnPin(int handle) {
    const Channel *channel = &channels[handle];
    hal::pwm::Output output = outputOf(handle);
    twin::pwmDrive(channel->pin, channel->invert, output.frequency, output.duty);
}

} // namespace

namespace twin {

/**
 * Reads "pwm" of the description: its four numbers, all required.
 */
bool pwmRead(JSContext *ctx, JSValueConst description, const char *path) {
    JSValue pwm = JS_GetPropertyStr(ctx, description, "pwm");
    if (JS_IsUndefined(pwm)) {
        return true;
    }

    // the outputs, the timers, their clock and counter
    double outputs = 0;
    double timerCount = 0;
    double clock = 0;
    double bits = 0;
    bool valid = readNumber(ctx, pwm, "outputs", 1, MAX_OUTPUTS, &outputs) &&
                 readNumber(ctx, pwm, "timers", 1, hal::pwm::MAX_TIMERS, &timerCount) &&
                 readNumber(ctx, pwm, "clock", 1, 1e9, &clock) &&
                 readNumber(ctx, pwm, "maxBits", 1, MAX_BITS, &bits);
    JS_FreeValue(ctx, pwm);
    if (!valid) {
        fprintf(stderr, "qjsx agent: %s: \"pwm\" is { outputs: 1 to %d, timers: 1 to %d, clock: Hz, maxBits: 1 to %d }\n",
                path, MAX_OUTPUTS, hal::pwm::MAX_TIMERS, MAX_BITS);
        return false;
    }

    // kept
    outputCount = (int)outputs;
    timers.count = (int)timerCount;
    clockHz = (uint32_t)clock;
    timerBits = (int)bits;
    return true;
}

} // namespace twin

namespace hal {
namespace pwm {

/**
 * Opens an output:
 * 1. a pin of the board, a frequency within reach;
 * 2. the pin not an output yet, a free output;
 * 3. the timer of that frequency, or a free one;
 * 4. the pin told its signal.
 */
Opened open(int pin, uint32_t frequency, double duty, bool invert) {
    // a pin of the board, a frequency the timers can make
    TimerSetting setting = timerSetting(clockHz, frequency, timerBits);
    if (outputCount == 0 || pin < 0 || pin >= twin::pinCount() || setting.bits == 0) {
        return Opened{ -1, EINVAL, {} };
    }

    // the pin free, a free output
    int free = -1;
    for (int i = 0; i < outputCount; i++) {
        if (channels[i].used && channels[i].pin == pin) {
            return Opened{ -1, EBUSY, {} };
        }
        if (!channels[i].used && free < 0) {
            free = i;
        }
    }
    if (free < 0) {
        return Opened{ -1, EBUSY, {} };
    }

    // its timer
    bool fresh = false;
    int timer = timerTake(&timers, frequency, &fresh);
    if (timer < 0) {
        return Opened{ -1, EBUSY, {} };
    }
    if (fresh) {
        settings[timer] = setting;
    }

    // the output, its pin told
    channels[free] = Channel{ true, pin, timer, invert, duty };
    showOnPin(free);
    return Opened{ free, 0, outputOf(free) };
}

/**
 * Stops an output: the pin a plain output at its idle level; its timer left.
 */
void close(int handle) {
    Channel *channel = channelOf(handle);
    if (channel == nullptr) {
        return;
    }

    twin::pwmStop(channel->pin, channel->invert);
    timerLeave(&timers, channel->timer);
    *channel = Channel{};
}

/**
 * Another frequency: the output moves to the timer of that frequency (its
 * own when alone on it), the duty cycle on the new resolution.
 */
int setFrequency(int handle, uint32_t frequency, Output *out) {
    Channel *channel = channelOf(handle);
    if (channel == nullptr) {
        return EINVAL;
    }

    // the same frequency: nothing to do
    int from = channel->timer;
    if (timers.timers[from].frequency == frequency) {
        *out = outputOf(handle);
        return 0;
    }

    // within reach
    TimerSetting setting = timerSetting(clockHz, frequency, timerBits);
    if (setting.bits == 0) {
        *out = outputOf(handle);
        return EINVAL;
    }

    // a timer for it
    bool fresh = false;
    int timer = timerMove(&timers, from, frequency, &fresh);
    if (timer < 0) {
        *out = outputOf(handle);
        return EBUSY;
    }
    if (fresh) {
        settings[timer] = setting;
    }

    // the output on it, its pin told
    channel->timer = timer;
    showOnPin(handle);
    *out = outputOf(handle);
    return 0;
}

/**
 * Another duty cycle.
 */
int setDuty(int handle, double duty, Output *out) {
    Channel *channel = channelOf(handle);
    if (channel == nullptr) {
        return EINVAL;
    }

    channel->duty = duty;
    showOnPin(handle);
    *out = outputOf(handle);
    return 0;
}

} // namespace pwm
} // namespace hal
