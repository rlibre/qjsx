/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "device/board": what belongs to the board the program runs on
// (board.h), on the devices, the desktop agent and the twin.
//
//   import { id, mac, restart } from "device/board";
//   id();            // the unique id of the chip, a Uint8Array (ESP32: its base MAC)
//   mac("wifi");     // a MAC address, 6 bytes: "base", "wifi", "ap", "bluetooth", "ethernet"
//   restart();       // restarts the chip; never returns
//
// restart() does what the agent's RESET does (rt::requestRestart, set by the
// device's runner): on a target, the pending output is sent (at most
// board::RESTART_FLUSH_MS), then the chip restarts; on the desktop agent and
// the twin, the installed program runs again (a package in RAM is lost).
// Nothing more of the program runs: it ends here, as with process.exit().
#include <stdio.h>

#include "jsbind.h"
#include "board.h"
#include "loop.h"
#include "restart.h"

namespace {

JS_ENUM(MacKind, base, wifi, ap, bluetooth, ethernet);

/**
 * board.id(): the unique id of the chip, in a new Uint8Array.
 */
js::Buffer id() {
    uint8_t bytes[board::MAX_CHIP_ID];
    size_t size = board::chipId(bytes);
    return js::Buffer::copy(bytes, size);
}

/**
 * board.mac(kind = "base"): a MAC address of the chip, in a new 6-byte
 * Uint8Array. Throws when the chip has none of that kind.
 */
js::Buffer mac(js::Optional<MacKind> kind) {
    // the kind asked, as board.h knows it
    MacKind asked = kind.valueOr(MacKind::base);
    board::MacKind kinds[] = {
        board::MacKind::base, board::MacKind::wifi, board::MacKind::ap, board::MacKind::bluetooth, board::MacKind::ethernet,
    };
    board::MacKind boardKind = kinds[(int)asked];

    // its address
    uint8_t bytes[6];
    if (!board::macAddress(boardKind, bytes)) {
        return js::fail("board.mac(): this chip has no MAC address of that kind");
    }
    return js::Buffer::copy(bytes, sizeof(bytes));
}

/**
 * board.restart(): restarts the device (see the top of the file). On a target
 * the call does not return; elsewhere the program ends now, with an
 * exception no catch or finally sees (as process.exit()).
 */
void restart() {
    // what the program wrote comes out first
    fflush(stdout);
    fflush(stderr);

    // the device's runner restarts it; without one (qjsx run) there is no
    // device to restart
    if (!rt::requestRestart()) {
        return (void)js::fail("board.restart(): only a device can restart (a board, the desktop agent, the twin)");
    }

    // the desktop: end this run now; the runner runs the installed program
    // again (the loop drops the exception after exit())
    rt::Loop::current()->exit(0);
    JSContext *ctx = js::context();
    JS_ThrowInternalError(ctx, "board.restart");
    JS_SetUncatchableException(ctx, true);
    js::detail::markPendingException();
}

} // namespace

/**
 * Module "device/board": id(), mac(), restart().
 */
JS_MODULE(board, module) {
    JS_FUNCTION(module, id);
    JS_FUNCTION(module, mac);
    JS_FUNCTION(module, restart);
}
