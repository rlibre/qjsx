/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The restart of the device, asked by its program (board.restart() of
// device/board). The device's runner (common/agent/runner.cpp) sets the
// handler: it does what the agent's RESET does, a restart of the chip on a
// target (does not return), the installed program run again on the desktop
// agent and the twin. Without a device runner (qjsx run), there is none.
#pragma once

namespace rt {

typedef void RestartHandler(void *opaque);

/**
 * Sets the handler of restart requests ('handler' nullptr: none). Called
 * once, before the programs run.
 */
void setRestartHandler(RestartHandler *handler, void *opaque);

/**
 * Asks for a restart, from the JS thread: does not return on a target; on
 * the desktop the installed program will run again once this run ends.
 * False when there is no handler (not a device). The twin also calls it
 * from the thread of its page (the reset button of a board): the handler does what
 * the agent's RESET does from its transport thread.
 */
bool requestRestart();

} // namespace rt
