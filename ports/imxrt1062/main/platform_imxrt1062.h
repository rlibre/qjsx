/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What the i.MX RT1062 port adds to platform.h for main().
#pragma once

namespace platform {

/**
 * Starts the 64-bit clock of monotonicMs() (the PIT's lifetime timer:
 * channels 0 and 1 chained, at 24 MHz). Called once by main(), after the
 * clocks, before anything reads the time.
 */
void startClock();

} // namespace platform
