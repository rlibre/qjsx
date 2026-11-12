/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The driver of device/serial for the serial ports of the PC (hal_serial.h):
// Windows COM ports, POSIX ttys.
#pragma once

#include "hal_serial.h"

namespace host {

/**
 * The driver of the PC's serial ports (static).
 */
const hal::serial::Driver *serialDriver();

} // namespace host
