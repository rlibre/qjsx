/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The USB serial port of the i.MX RT1062 (USB1, a CDC ACM function of NXP's
// USB device stack in bare-metal mode): the host link of the agent and the
// console. Bytes flow through two rings: the USB interrupt fills the receive
// ring and empties the send ring; tasks do the rest.
//
// A host that sets the port to 134 bauds asks for the Teensy loader (PJRC's
// convention, agreed 2026-11-16): the firmware jumps into it shortly after
// (loader_imxrt1062.h), even if the program or the agent hangs.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace usbserial {

/**
 * Starts the USB device: clocks, PHY, the stack, its interrupt. Called once
 * by main(), before the scheduler (the receive signal is created here).
 */
void   start();

/**
 * Takes up to 'size' received bytes into 'buffer'; returns how many (0 if
 * none). One reader task only.
 */
size_t read(uint8_t *buffer, size_t size);

/**
 * Waits until bytes were received, at most 'timeoutMs'. False on timeout.
 * The reader task only.
 */
bool   waitReceived(uint32_t timeoutMs);

/**
 * Sends 'size' bytes, whole: the call holds the write lock, so frames and
 * console text never mix. Without a host, nothing is sent; what the host does
 * not take within WRITE_TIMEOUT_MS is dropped. False if bytes were dropped.
 * Any task (not an interrupt).
 */
bool   write(const uint8_t *data, size_t size);

/**
 * Waits until everything written was sent, at most 'timeoutMs'.
 */
void   flush(uint32_t timeoutMs);

const uint32_t WRITE_TIMEOUT_MS = 50;   // what write() waits for room before dropping

} // namespace usbserial
