/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The PI4IOE5V6408 (pi4ioe5v6408.h). Its registers, one bit per pin (from
// its datasheet):
//   0x03 direction        1: output
//   0x05 output state     1: high
//   0x07 output high-Z    1: not driven
//   0x0B pull enable      1: a pull resistor on the pin
//   0x0D pull select      1: pull-up, 0: pull-down
#include "pi4ioe5v6408.h"

#include <stdint.h>

#include "hal_i2c.h"

namespace drivers {
namespace pi4ioe5v6408 {

namespace {

const uint8_t DIRECTION = 0x03;
const uint8_t OUTPUT_STATE = 0x05;
const uint8_t OUTPUT_HIGH_Z = 0x07;
const uint8_t PULL_ENABLE = 0x0B;
const uint8_t PULL_SELECT = 0x0D;

const int TIMEOUT_MS = 50;   // of one transfer

/**
 * Sets bit 'pin' of register 'reg' to 'value', the other bits kept. False
 * on an I2C failure.
 */
bool setBit(int device, uint8_t reg, int pin, bool value) {
    // the register as it is
    uint8_t current = 0;
    if (hal::i2c::transfer(device, &reg, 1, &current, 1, TIMEOUT_MS) != hal::i2c::Status::ok) {
        return false;
    }

    // its bit changed, written back
    uint8_t mask = (uint8_t)(1 << pin);
    uint8_t changed = value ? (uint8_t)(current | mask) : (uint8_t)(current & ~mask);
    uint8_t write[2] = { reg, changed };
    return hal::i2c::transfer(device, write, 2, nullptr, 0, TIMEOUT_MS) == hal::i2c::Status::ok;
}

} // namespace

bool drive(int device, int pin, bool high) {
    // the level first, driven, then the pin an output: no glitch
    return setBit(device, OUTPUT_STATE, pin, high)
           && setBit(device, OUTPUT_HIGH_Z, pin, false)
           && setBit(device, DIRECTION, pin, true);
}

bool release(int device, int pin) {
    // its pull-up, then the pin an input
    return setBit(device, PULL_SELECT, pin, true)
           && setBit(device, PULL_ENABLE, pin, true)
           && setBit(device, DIRECTION, pin, false);
}

} // namespace pi4ioe5v6408
} // namespace drivers
