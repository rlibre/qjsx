/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// hal::adc of the digital twin (see twin.h): the analog inputs the board
// description lists; a read converts the voltage on the pin (what a
// component sets, a potentiometer; else its level times vref) over the span
// chosen for the range, as the converter of the board would: above the span
// it saturates.
//
// Threads: the JS thread (hal::adc); the voltages come from twin.cpp, under
// its lock.
#include "hal_adc.h"
#include "twin.h"

#include <errno.h>

namespace {

const int MAX_INPUTS = 16;           // inputs open at the same time

// An open input.
struct Input {
    bool used;                       // open() gave it
    int pin;                         // its pin
    int spanMv;                      // what it reads, 0 to spanMv
};

Input inputs[MAX_INPUTS];            // JS thread only

/**
 * An open input by its handle; nullptr if there is none.
 */
Input *inputOf(int handle) {
    if (handle < 0 || handle >= MAX_INPUTS || !inputs[handle].used) {
        return nullptr;
    }
    return &inputs[handle];
}

/**
 * The voltage on the pin of an input, saturated at its span, in mV.
 */
int clampedVoltage(const Input *input) {
    int millivolts = twin::pinVoltage(input->pin);
    return millivolts > input->spanMv ? input->spanMv : millivolts;
}

} // namespace

namespace hal {
namespace adc {

/**
 * Opens the analog input of a pin: one of the board's, free, for a range it
 * reads.
 */
Opened open(int pin, int rangeMv) {
    // a pin with an analog input, a range the converter reads
    int span = twin::adcSpanFor(rangeMv);
    if (!twin::adcPin(pin) || span == 0) {
        return Opened{ -1, EINVAL, 0, 0 };
    }

    // not open yet, a free entry
    int free = -1;
    for (int i = 0; i < MAX_INPUTS; i++) {
        if (inputs[i].used && inputs[i].pin == pin) {
            return Opened{ -1, EBUSY, 0, 0 };
        }
        if (!inputs[i].used && free < 0) {
            free = i;
        }
    }
    if (free < 0) {
        return Opened{ -1, EBUSY, 0, 0 };
    }

    inputs[free] = Input{ true, pin, span };
    twin::setAdc(pin, true);
    return Opened{ free, 0, twin::adcBits(), span };
}

/**
 * Closes an input.
 */
void close(int handle) {
    Input *input = inputOf(handle);
    if (input == nullptr) {
        return;
    }

    twin::setAdc(input->pin, false);
    *input = Input{};
}

/**
 * A raw conversion: the voltage over the span, on the bits of the
 * converter.
 */
int readRaw(int handle) {
    Input *input = inputOf(handle);
    if (input == nullptr) {
        return -1;
    }

    int maxRaw = (1 << twin::adcBits()) - 1;
    return (int)((int64_t)clampedVoltage(input) * maxRaw / input->spanMv);
}

/**
 * A conversion in millivolts: the voltage itself (the twin's converter is
 * exact), saturated at the span.
 */
int readMillivolts(int handle) {
    Input *input = inputOf(handle);
    if (input == nullptr) {
        return -1;
    }
    return clampedVoltage(input);
}

} // namespace adc
} // namespace hal
