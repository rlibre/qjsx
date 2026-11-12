/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Analog inputs: what the module device/adc needs from the hardware (the
// "hal"). A pin with an analog input is opened for the highest voltage the
// program expects on it; the hal chooses the setting of the converter that
// covers it (the attenuation of the ESP32; nothing on the RP2350, 0 to 3.3
// V). A read is synchronous and short (a few microseconds). One
// implementation per port (ports/esp32/main/hal_adc_esp32.cpp: ADC1 only),
// and the digital twin's (host/twin/sim/twin_adc.cpp).
//
// Threads: every function is called on the JS thread.
#pragma once

#include <stdint.h>

namespace hal {
namespace adc {

// What open() gives: the handle of the input, or why it failed.
struct Opened {
    int handle;              // >= 0 when opened; -1 when not
    int error;               // errno when not opened: EINVAL (no analog input on the pin, or a
                             // range it cannot read), EBUSY (already open)
    int bits;                // resolution of the raw values (12: 0 to 4095)
    int rangeMv;             // the highest voltage it reads, in millivolts (what the setting covers)
};

/**
 * Opens the analog input of 'pin' for voltages up to 'rangeMv'.
 */
Opened open(int pin, int rangeMv);

/**
 * Closes an input opened by open().
 */
void close(int handle);

/**
 * A raw conversion: 0 to 2^bits - 1; -1 if it failed.
 */
int readRaw(int handle);

/**
 * A conversion in millivolts, corrected by the calibration of the chip; -1
 * if it failed.
 */
int readMillivolts(int handle);

} // namespace adc
} // namespace hal
