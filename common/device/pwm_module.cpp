/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "device/pwm": the PWM outputs of the board (hal_pwm.h), on the
// boards whose port implements it (registered by the port, see
// device_modules.h).
//
//   import { Pwm } from "device/pwm";
//   const led = new Pwm(5, { frequency: 1000, duty: 0, invert: false });
//   led.setDuty(0.25);          // duty cycle, 0 to 1 (0 and 1: really off, really on)
//   led.setFrequency(50);       // Hz; the duty cycle is kept
//   led.setPulseWidth(1500);    // µs active per period (servos), as a duty cycle
//   led.duty; led.frequency;    // what the hardware does (rounded)
//   led.bits;                   // resolution of the duty cycle
//   led.close();
//
// Everything is synchronous. The resolution is the highest the frequency
// allows (chosen by the hal). 'invert': active low, for a LED wired to the
// supply (the pin is 1 at rest). Closed, or at the end of the program (the
// finalizer), the output stops: the pin stays driven at its idle level, the
// timer is freed (user, 2026-11-13: a motor must not keep running).
#include <errno.h>
#include <stdio.h>

#include "jsbind.h"
#include "hal_pwm.h"

namespace {

// The frequencies accepted from JS, in Hz (the hal refuses those its
// hardware cannot make).
const int LOWEST_FREQUENCY = 1;
const int HIGHEST_FREQUENCY = 40000000;
typedef js::Range<int, LOWEST_FREQUENCY, HIGHEST_FREQUENCY> Frequency;

// Options of 'new Pwm(pin, options)'.
struct PwmOptions {
    Frequency frequency = 1000;                  // Hz
    double    duty = 0;                          // 0 to 1, the share of a period the output is active
    bool      invert = false;                    // active low: the pin is 1 at rest
};
JS_STRUCT(PwmOptions, frequency, duty, invert);

/**
 * Whether a duty cycle is one: 0 to 1 (NaN is not).
 */
bool isDuty(double value) {
    return value >= 0.0 && value <= 1.0;
}

/**
 * A PWM output (JS class Pwm).
 */
class Pwm {
    JS_CLASS(Pwm);

public:
    /**
     * Finalizer (no JS here): stops the output.
     */
    ~Pwm() {
        closeOutput();
    }

    /**
     * JS constructor: new Pwm(pin, { frequency, duty, invert }) starts the
     * output. Throws a RangeError (a duty cycle out of 0 to 1), EINVAL (no
     * output on the pin, a frequency out of reach) or EBUSY (the pin is an
     * output already, no output or timer free for that frequency).
     */
    void init(js::Range<int, 0, 63> pin, js::Optional<PwmOptions> options) {
        // the options, checked
        PwmOptions settings = options.has() ? options.value() : PwmOptions();
        if (!isDuty(settings.duty)) {
            return (void)js::fail<js::RangeError>("pwm: duty must be 0 to 1");
        }

        // the output
        uint32_t frequencyHz = (uint32_t)(int)settings.frequency;
        hal::pwm::Opened opened = hal::pwm::open((int)pin, frequencyHz, settings.duty, settings.invert);
        if (opened.handle < 0) {
            char what[96];
            snprintf(what, sizeof(what), "pwm: pin %d at %u Hz", (int)pin, (unsigned)frequencyHz);
            return (void)js::failSystem(opened.error, what);
        }

        _handle = opened.handle;
        show(&opened.output);
    }

    /**
     * JS: another duty cycle, 0 to 1, from the next period. Throws if closed,
     * or a RangeError.
     */
    JS_EXPORT(setDuty) void setDuty(double value) {
        if (!isDuty(value)) {
            return (void)js::fail<js::RangeError>("pwm: duty must be 0 to 1");
        }
        applyDuty(value);
    }

    /**
     * JS: another frequency, the duty cycle kept. Throws if closed, EINVAL
     * (out of reach) or EBUSY (no timer free for it: the output keeps its
     * frequency).
     */
    JS_EXPORT(setFrequency) void setFrequency(Frequency hz) {
        if (!isOpen()) {
            return (void)js::fail("pwm: closed");
        }

        hal::pwm::Output output;
        int error = hal::pwm::setFrequency(_handle, (uint32_t)(int)hz, &output);
        if (error != 0) {
            char what[64];
            snprintf(what, sizeof(what), "pwm: %d Hz", (int)hz);
            return (void)js::failSystem(error, what);
        }
        show(&output);
    }

    /**
     * JS: the time the output is active in each period, in µs (a servo), at
     * the real frequency: set as a duty cycle. Throws if closed, or a
     * RangeError (negative, or longer than the period).
     */
    JS_EXPORT(setPulseWidth) void setPulseWidth(double microseconds) {
        double value = microseconds * 1e-6 * frequency;
        if (!(microseconds >= 0.0) || !isDuty(value)) {
            return (void)js::fail<js::RangeError>("pwm: the pulse must last 0 to one period");
        }
        applyDuty(value);
    }

    /**
     * JS: stops the output (the pin stays at its idle level); the setters
     * throw after it. Closing twice does nothing.
     */
    JS_EXPORT(close) void close() {
        closeOutput();
    }

    // What the hardware does (set by each call; writing them changes
    // nothing).
    JS_EXPORT(duty) double duty = 0;             // 0 to 1, rounded to a step of the resolution
    JS_EXPORT(frequency) double frequency = 0;   // Hz, rounded by the hardware
    JS_EXPORT(bits) int bits = 0;                // resolution of the duty cycle

private:
    /**
     * Whether the output runs.
     */
    bool isOpen() const {
        return _handle >= 0;
    }

    /**
     * Gives a duty cycle (checked) to the hal; throws if closed.
     */
    void applyDuty(double value) {
        if (!isOpen()) {
            return (void)js::fail("pwm: closed");
        }

        hal::pwm::Output output;
        int error = hal::pwm::setDuty(_handle, value, &output);
        if (error != 0) {
            return (void)js::failSystem(error, "pwm: setDuty");
        }
        show(&output);
    }

    /**
     * Shows what the hardware does in the JS fields.
     */
    void show(const hal::pwm::Output *output) {
        duty = output->duty;
        frequency = output->frequency;
        bits = output->bits;
    }

    /**
     * Stops the output, once.
     */
    void closeOutput() {
        if (_handle >= 0) {
            hal::pwm::close(_handle);
            _handle = -1;
        }
    }

    int _handle = -1;            // of hal::pwm; -1: closed
};

} // namespace

/**
 * Module "device/pwm".
 */
JS_MODULE(pwm, module) {
    JS_ADD_CLASS(module, Pwm);
}
