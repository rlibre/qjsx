/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The way into the Teensy's loader (HalfKay, in the board's second chip): a
// "bkpt #251" halts the core, the loader chip sees it and takes the board
// for programming (PJRC's convention). Asked from the USB interrupt (a host
// set 134 bauds), done a little later by a timer interrupt (PIT channel 2),
// so that the USB stack first answers the request: independent of FreeRTOS
// and of the program.
#pragma once

namespace loader {

/**
 * Enters the loader in ENTER_DELAY_MS, from a timer interrupt. Callable from
 * an interrupt. platform::startClock() must have started the PIT.
 */
void enterSoon();

const unsigned ENTER_DELAY_MS = 20;   // lets the USB answer the request first

} // namespace loader
