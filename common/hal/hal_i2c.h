/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// I2C master: what the module device/i2c needs from the hardware (the
// "hal"). A bus is a controller of the chip on two pins; a device is an
// address on a bus with its own clock frequency. One implementation per port
// (ports/esp32/main/hal_i2c_esp32.cpp) and one in the digital twin
// (host/twin/sim/twin_i2c.cpp, where no address answers).
//
// Every function blocks until it is done. Threads: any thread, but never two
// calls on the same bus at once (the module sees to it: one transfer of a bus
// at a time, from its "i2c" task or from the JS thread).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hal {
namespace i2c {

// How a transfer or a probe ended.
enum class Status {
    ok,                 // done
    noAck,              // nobody answered at the address, or a byte was refused (NACK)
    timeout,            // not done within the time given
    failed,             // any other error of the hardware or the driver
};

/**
 * Number of buses (controllers) of the chip: bus numbers 0 to busCount() - 1.
 */
int    busCount();

/**
 * Takes bus 'bus' on pins 'sda' and 'scl', with the chip's pull-ups if
 * 'pullUp'. False if the bus or a pin is refused.
 */
bool   openBus(int bus, int sda, int scl, bool pullUp);

/**
 * Frees the bus and its pins. Its devices must be removed first.
 */
void   closeBus(int bus);

/**
 * Adds the device of 7-bit address 'address' on the open bus 'bus', clocked
 * at 'frequencyHz'. Returns its number, -1 if refused.
 */
int    addDevice(int bus, int address, uint32_t frequencyHz);

/**
 * Removes a device added by addDevice().
 */
void   removeDevice(int device);

/**
 * One transfer with a device: writes 'writeSize' bytes, then reads
 * 'readSize' bytes after a repeated start (either size may be 0, not both),
 * within 'timeoutMs'.
 */
Status transfer(int device, const uint8_t *writeData, size_t writeSize, uint8_t *readData, size_t readSize,
                int timeoutMs);

/**
 * Whether something answers at 'address' on the open bus 'bus' (ok) or not
 * (noAck), within 'timeoutMs'.
 */
Status probe(int bus, int address, int timeoutMs);

/**
 * Frees a bus that a device holds stuck (clock pulses, then a stop).
 */
void   resetBus(int bus);

} // namespace i2c
} // namespace hal
