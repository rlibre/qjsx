/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What the module device/spi (spi_module.cpp) offers to the other native
// modules: a transaction on a device the program opened, made of hal
// segments (hal_spi.h) the module builds itself, such as a segment whose
// bytes are made piece by piece (sys/draw: the pixels of a display).
#pragma once

#include <stddef.h>

#include "hal_spi.h"
#include "jsbind.h"

namespace device {

/**
 * Queues a transaction of 'count' segments on 'deviceValue', a JS SpiDevice, in
 * the order of the calls on its bus, like its own write(). On the JS thread,
 * inside a JS call.
 *
 * The segments are taken on every path: the array, the 'write' and 'read'
 * bytes, the 'makeOpaque' and 'piece' of a made segment, all from
 * memory::allocate, are freed once the transaction is over or refused.
 *
 * Returns its promise, resolved with undefined when it is over; rejected
 * with EBADF (device or bus closed), EAGAIN (too many pending), EIO; an
 * empty value with a TypeError recorded when it is not an SpiDevice.
 */
js::Value queueSpiSegments(js::Value deviceValue, hal::spi::Segment *segments, size_t count);

} // namespace device
