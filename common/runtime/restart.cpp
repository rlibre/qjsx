/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The restart of the device asked by its program (see restart.h).
#include "restart.h"

#include <stddef.h>

namespace {

rt::RestartHandler *restartHandler = nullptr;   // set once, before the programs run
void *restartOpaque = nullptr;                  // passed to it

} // namespace

namespace rt {

/**
 * Keeps the handler and its argument.
 */
void setRestartHandler(RestartHandler *handler, void *opaque) {
    restartHandler = handler;
    restartOpaque = opaque;
}

/**
 * Calls the handler, if any.
 */
bool requestRestart() {
    if (restartHandler == nullptr) {
        return false;
    }

    restartHandler(restartOpaque);
    return true;
}

} // namespace rt
