/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Analog outputs: what the module device/dac needs from the hardware (the
// "hal"). A pin with a DAC (digital to analog converter) is opened, then
// driven with raw codes, 0 to 2^bits - 1: code c gives c / (2^bits - 1) of
// the full scale, the voltage of the highest code (from the board's
// description). A write is synchronous and short. Closed, the output stops
// and the pin is given back (as a pin nobody configured).
//
// Only some chips have a DAC: the original ESP32 (GPIO25 / 26) and the
// ESP32-S2 (GPIO17 / 18), 8 bits. One implementation per port that has one
// (ports/esp32/main/hal_dac_esp32.cpp, those two chips only), and the
// digital twin's (host/twin/sim/twin_dac.cpp).
//
// Threads: every function is called on the JS thread.
#pragma once

namespace hal {
namespace dac {

// What open() gives: the handle of the output, or why it failed.
struct Opened {
    int handle;              // >= 0 when opened; -1 when not
    int error;               // errno when not opened: EINVAL (no DAC on the pin), EBUSY (open
                             // already), EIO (the hardware refused it)
    int bits;                // resolution: codes 0 to 2^bits - 1 (8 on the ESP32)
    int fullScaleMv;         // the voltage of the highest code, in millivolts
};

/**
 * Opens the DAC of 'pin'; its output starts at code 0.
 */
Opened open(int pin);

/**
 * Drives an output opened by open() with code 'raw' (0 to 2^bits - 1,
 * checked by the caller). 0, or errno: EINVAL (no such output), EIO (the
 * hardware refused it).
 */
int write(int handle, int raw);

/**
 * Stops an output opened by open() and gives its pin back.
 */
void close(int handle);

} // namespace dac
} // namespace hal
