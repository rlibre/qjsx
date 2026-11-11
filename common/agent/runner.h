/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The device side, the same on every board: the agent (protocol), the
// debugger stub and the runner of the installed package, around board.h.
//
//   board::configure...()        the port prepares its board
//   agent::runDevice(identity)   never returns
//
// Threads: the calling thread becomes the JS thread (it runs the packages);
// the links of the board call the agent from their own threads.
#pragma once

#include "agent.h"

namespace agent {

/**
 * Runs the device: starts the links, runs the installed package at boot,
 * then runs the packages the host asks for (RUN, RESET), forever. The
 * calling thread becomes the JS thread. Returns only if the links of the
 * board cannot be started.
 */
void runDevice(Identity identity);

} // namespace agent
