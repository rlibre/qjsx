/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The PI4IOE5V6408 (Diodes), an expander of 8 pins on I2C (2026-11-18, first
// for the M5Stack Tab5, where the reset of its display goes through one).
// Portable, over hal_i2c.h: the caller opens the bus and adds the device.
// Each pin is changed alone (its register read, its bit changed, written
// back): the other pins keep what the board set.
#pragma once

namespace drivers {
namespace pi4ioe5v6408 {

/**
 * Drives pin 'pin' (0..7) of the expander 'device' (hal::i2c::addDevice)
 * to 'high' or low, as a push-pull output. False on an I2C failure.
 */
bool drive(int device, int pin, bool high);

/**
 * Releases pin 'pin': an input with its pull-up, so that what is wired to
 * it is high unless something else drives it. False on an I2C failure.
 */
bool release(int device, int pin);

} // namespace pi4ioe5v6408
} // namespace drivers
