/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// hal::dac of the digital twin (see twin.h): the DAC outputs the board
// description lists ("dac": { "pins", "bits", "fullScale" }); a code drives
// its pin at code / (2^bits - 1) of the full scale, which the parts and the
// page read as the voltage of the pin (twin.cpp: dacDrive). The twin's DAC
// is exact: no offset, no non-linearity.
//
// Threads: the description is read by load() before any thread; hal::dac
// on the JS thread; the pins' voltages live in twin.cpp, under its lock.
#include "hal_dac.h"
#include "twin.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>

#include "quickjs.h"

namespace {

const int MAX_CHANNELS = 8;          // DAC outputs a board may have
const int MAX_BITS = 16;             // the widest DAC a board may have
const double MAX_FULL_SCALE = 30.0;  // volts: the twin keeps voltages in mV on 16 bits

// The DAC of the board ("dac" of the description; written by load()).
bool present = false;                // the description has "dac"
int channelPins[MAX_CHANNELS];       // the pin of each channel
int channelCount = 0;                // entries of channelPins
int converterBits = 8;               // "dac.bits"
int fullScaleMv = 3300;              // "dac.fullScale" in mV: the voltage of the highest code

// The open outputs, by channel (the handle is the channel's index).
bool used[MAX_CHANNELS];             // JS thread only

/**
 * The channel on 'pin'; -1 if the pin has no DAC.
 */
int channelOf(int pin) {
    for (int i = 0; i < channelCount; i++) {
        if (channelPins[i] == pin) {
            return i;
        }
    }

    return -1;
}

/**
 * Whether 'handle' is an open output.
 */
bool isOpen(int handle) {
    bool inTable = handle >= 0 && handle < channelCount;
    return inTable && used[handle];
}

/**
 * The voltage of a code, in mV (rounded to the mV).
 */
int millivoltsOf(int raw) {
    int highestCode = (1 << converterBits) - 1;
    double share = (double)raw / highestCode;
    return (int)lround(share * fullScaleMv);
}

/**
 * Reads the number 'name' of 'object' into '*out' when present; false when
 * it is not a number within 'low' to 'high'.
 */
bool readOptionalNumber(JSContext *ctx, JSValueConst object, const char *name, double low, double high, double *out) {
    // absent: the default stays
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    if (JS_IsUndefined(value)) {
        return true;
    }

    // a number, within the bounds (false for NaN too)
    double number = 0;
    bool isNumber = JS_IsNumber(value) && JS_ToFloat64(ctx, &number, value) == 0;
    JS_FreeValue(ctx, value);
    bool valid = isNumber && number >= low && number <= high;
    if (valid) {
        *out = number;
    }

    return valid;
}

} // namespace

namespace twin {

/**
 * Reads "dac" of the description: its pins (required), bits and full scale
 * (optional: 8, 3.3 V).
 */
bool dacRead(JSContext *ctx, JSValueConst description, const char *path) {
    // no "dac": the board has no DAC
    JSValue dac = JS_GetPropertyStr(ctx, description, "dac");
    if (JS_IsUndefined(dac)) {
        return true;
    }

    // the pins, the bits, the full scale
    channelCount = readIntegers(ctx, dac, "pins", 0, pinCount() - 1, channelPins, MAX_CHANNELS);
    double bits = converterBits;
    double fullScale = fullScaleMv / 1000.0;
    bool bitsValid = readOptionalNumber(ctx, dac, "bits", 1, MAX_BITS, &bits) && bits == floor(bits);
    bool fullScaleValid = readOptionalNumber(ctx, dac, "fullScale", 0.001, MAX_FULL_SCALE, &fullScale);
    JS_FreeValue(ctx, dac);

    // all of them right, else what is expected
    bool pinsValid = channelCount >= 1;
    if (!pinsValid || !bitsValid || !fullScaleValid) {
        fprintf(stderr, "qjsx agent: %s: \"dac\" is { pins: [1 to %d pin numbers], bits: 1 to %d, fullScale: volts "
                "(up to %g) }\n", path, MAX_CHANNELS, MAX_BITS, MAX_FULL_SCALE);
        return false;
    }

    // kept
    converterBits = (int)bits;
    fullScaleMv = (int)lround(fullScale * 1000.0);
    present = true;
    return true;
}

/**
 * Whether the board has a DAC.
 */
bool dacPresent() {
    return present;
}

} // namespace twin

namespace hal {
namespace dac {

/**
 * Opens the DAC of a pin: one of the board's, not open yet; its output at
 * code 0.
 */
Opened open(int pin) {
    // a pin with a DAC, free
    int channel = channelOf(pin);
    if (channel < 0) {
        return Opened{ -1, EINVAL, 0, 0 };
    }

    if (used[channel]) {
        return Opened{ -1, EBUSY, 0, 0 };
    }

    // taken, at 0 V
    used[channel] = true;
    twin::dacDrive(pin, 0);
    return Opened{ channel, 0, converterBits, fullScaleMv };
}

/**
 * Drives a code: its voltage on the pin.
 */
int write(int handle, int raw) {
    if (!isOpen(handle)) {
        return EINVAL;
    }

    twin::dacDrive(channelPins[handle], millivoltsOf(raw));
    return 0;
}

/**
 * Stops an output: the pin given back.
 */
void close(int handle) {
    if (!isOpen(handle)) {
        return;
    }

    used[handle] = false;
    twin::dacStop(channelPins[handle]);
}

} // namespace dac
} // namespace hal
