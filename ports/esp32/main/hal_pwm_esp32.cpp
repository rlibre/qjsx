/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// PWM outputs on the ESP32 (hal_pwm.h), with the LEDC driver of ESP-IDF:
//   - low speed mode (the only one of the S3), the APB clock (80 MHz), so
//     that the twin can compute the same frequencies (pwm_timers.h);
//   - an output is a channel of LEDC (8), on any output pin through the
//     GPIO matrix; the outputs of the same frequency share a timer (4);
//   - the resolution is the highest the frequency allows, at most what the
//     timers of the chip count (14 bits on the S3);
//   - inverted outputs are inverted by the GPIO matrix (output_invert);
//   - closed: the channel stops at its idle level, then the pin becomes a
//     plain GPIO output at that same level (still driven, no glitch); a
//     timer nobody uses any more is paused.
#include "hal_pwm.h"
#include "pwm_timers.h"

#include <errno.h>

#include "driver/gpio.h"
#include "driver/ledc.h"

namespace {

const ledc_mode_t MODE = LEDC_LOW_SPEED_MODE;     // the only mode of the S3
const uint32_t CLOCK = 80000000;                  // the APB clock, Hz
const int TIMER_COUNT = LEDC_TIMER_MAX;           // timers of the chip (4)
const int CHANNEL_COUNT = LEDC_CHANNEL_MAX;       // outputs of the chip (8 on the S3)
const int MAX_BITS = LEDC_TIMER_BIT_MAX - 1;      // the widest counter of the chip (14 bits on the S3)

// An output: a channel of LEDC.
struct Channel {
    bool   used;                                  // open() gave it
    int    pin;                                   // its GPIO
    int    timer;                                 // the timer it runs on
    bool   invert;                                // active low
    double duty;                                  // the duty cycle asked last, 0 to 1
};

Channel channels[CHANNEL_COUNT];
hal::pwm::TimerTable timers = { {}, TIMER_COUNT };
hal::pwm::TimerSetting settings[TIMER_COUNT];     // how each timer in use makes its frequency

/**
 * An open output by its handle; nullptr if there is none.
 */
Channel *channelOf(int handle) {
    if (handle < 0 || handle >= CHANNEL_COUNT || !channels[handle].used) {
        return nullptr;
    }
    return &channels[handle];
}

/**
 * Sets up timer 'index' for 'setting' (its resolution and the frequency
 * asked) and runs it; false if LEDC refuses.
 */
bool setUpTimer(int index, uint32_t frequency, const hal::pwm::TimerSetting *setting) {
    ledc_timer_config_t config = {};
    config.speed_mode = MODE;
    config.duty_resolution = (ledc_timer_bit_t)setting->bits;
    config.timer_num = (ledc_timer_t)index;
    config.freq_hz = frequency;
    config.clk_cfg = LEDC_USE_APB_CLK;
    if (ledc_timer_config(&config) != ESP_OK) {
        return false;
    }

    // a timer paused when its last output closed runs again
    ledc_timer_resume(MODE, (ledc_timer_t)index);
    settings[index] = *setting;
    return true;
}

/**
 * Pauses timer 'index' when its last output left it.
 */
void leaveTimer(int index) {
    if (hal::pwm::timerLeave(&timers, index)) {
        ledc_timer_pause(MODE, (ledc_timer_t)index);
    }
}

/**
 * Gives the duty cycle of a channel to LEDC, on the resolution of its
 * timer, from the next period.
 */
void applyDuty(int handle) {
    Channel *channel = &channels[handle];
    uint32_t value = hal::pwm::dutySteps(channel->duty, settings[channel->timer].bits);
    ledc_set_duty(MODE, (ledc_channel_t)handle, value);
    ledc_update_duty(MODE, (ledc_channel_t)handle);
}

/**
 * What a channel does: the frequency of its timer, its duty cycle rounded to
 * a step.
 */
hal::pwm::Output outputOf(int handle) {
    const Channel *channel = &channels[handle];
    return hal::pwm::outputOn(&settings[channel->timer], channel->duty);
}

} // namespace

namespace hal {
namespace pwm {

/**
 * Opens an output:
 * 1. an output pin, a frequency within reach;
 * 2. the pin not an output yet, a free channel;
 * 3. the timer of that frequency, or a free one set up for it;
 * 4. the channel on the pin, at the duty cycle asked.
 */
Opened open(int pin, uint32_t frequency, double duty, bool invert) {
    Opened refused = { -1, EINVAL, {} };

    // an output pin, a frequency the timers can make
    TimerSetting setting = timerSetting(CLOCK, frequency, MAX_BITS);
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin) || setting.bits == 0) {
        return refused;
    }

    // the pin free, a free channel
    int free = -1;
    for (int i = 0; i < CHANNEL_COUNT; i++) {
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
    if (fresh && !setUpTimer(timer, frequency, &setting)) {
        leaveTimer(timer);
        return Opened{ -1, EIO, {} };
    }

    // the channel, on the pin
    channels[free] = Channel{ true, pin, timer, invert, duty };
    ledc_channel_config_t config = {};
    config.gpio_num = pin;
    config.speed_mode = MODE;
    config.channel = (ledc_channel_t)free;
    config.timer_sel = (ledc_timer_t)timer;
    config.duty = dutySteps(duty, settings[timer].bits);
    config.hpoint = 0;
    config.flags.output_invert = invert ? 1 : 0;
    if (ledc_channel_config(&config) != ESP_OK) {
        channels[free] = Channel{};
        leaveTimer(timer);
        return Opened{ -1, EIO, {} };
    }

    return Opened{ free, 0, outputOf(free) };
}

/**
 * Stops an output: the channel at its idle level, then the pin a plain
 * GPIO output at that level (set first: no glitch); its timer left.
 */
void close(int handle) {
    Channel *channel = channelOf(handle);
    if (channel == nullptr) {
        return;
    }

    // the channel at its idle level (before the inversion of the matrix)
    ledc_stop(MODE, (ledc_channel_t)handle, 0);

    // the pin driven by the GPIO at that level
    int idleLevel = channel->invert ? 1 : 0;
    gpio_set_level((gpio_num_t)channel->pin, idleLevel);
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << channel->pin;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&config);

    // its timer, the channel free
    leaveTimer(channel->timer);
    *channel = Channel{};
}

/**
 * Another frequency: the channel moves to the timer of that frequency (its
 * own when alone on it, set up again), the duty cycle given again on the new
 * resolution.
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
    TimerSetting setting = timerSetting(CLOCK, frequency, MAX_BITS);
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
        setUpTimer(timer, frequency, &setting);
    }

    // the channel on it; the timer it left paused when nobody uses it
    if (timer != from) {
        ledc_bind_channel_timer(MODE, (ledc_channel_t)handle, (ledc_timer_t)timer);
        if (timers.timers[from].users == 0) {
            ledc_timer_pause(MODE, (ledc_timer_t)from);
        }
    }
    channel->timer = timer;

    // the duty cycle on the new resolution
    applyDuty(handle);
    *out = outputOf(handle);
    return 0;
}

/**
 * Another duty cycle, from the next period.
 */
int setDuty(int handle, double duty, Output *out) {
    Channel *channel = channelOf(handle);
    if (channel == nullptr) {
        return EINVAL;
    }

    channel->duty = duty;
    applyDuty(handle);
    *out = outputOf(handle);
    return 0;
}

} // namespace pwm
} // namespace hal
