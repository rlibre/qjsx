/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "device/board": what belongs to the board the program runs on
// (board.h), on the devices, the desktop agent and the twin.
//
//   import { id, mac, restart, memory, load } from "device/board";
//   id();            // the unique id of the chip, a Uint8Array (ESP32: its base MAC)
//   mac("wifi");     // a MAC address, 6 bytes: "base", "wifi", "ap", "bluetooth", "ethernet"
//   restart();       // restarts the chip; never returns
//   memory();        // { jsHeap, jsHeapPeak, jsHeapSize, ramFree, ramLargest, ramLowest, psramFree }
//   load();          // { js, jsPeak, busy, elapsed }: the load of the JS thread
//
// memory() and load() give what "qjsx device info" shows: a value the board
// cannot tell is null (the RAM of the desktop agent and the twin, a JS heap
// without a fixed size).
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
#include "program.h"
#include "restart.h"

namespace {

/**
 * Sets property 'name' of 'object' to 'bytes' as a number, or null when
 * 'known' is false.
 */
void setSize(JSContext *ctx, JSValueConst object, const char *name, bool known, size_t bytes) {
    JSValue value = known ? JS_NewFloat64(ctx, (double)bytes) : JS_NULL;
    JS_SetPropertyStr(ctx, object, name, value);
}

JS_ENUM(MacKind, base, wifi, ap, bluetooth, ethernet);

/**
 * board.id(): the unique id of the chip, in a new Uint8Array.
 */
js::Buffer id() {
    // MAX_CHIP_ID bytes at most, copied into the array
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

/**
 * board.memory(): the JS heap of the program and the free memory of the
 * board, in bytes; null for what the board cannot tell.
 */
js::Value memoryReport() {
    JSContext *ctx = js::context();

    // the JS heap as the runtime measures it (always on a device)
    const rt::JsHeapUsage *heap = rt::currentHeapUsage();
    bool heapKnown = heap != nullptr;
    size_t heapLimit = board::jsHeapLimit();

    // the memory of the board (not known on the desktop agent and the twin)
    board::MemoryStatus status = {};
    bool statusKnown = board::memoryStatus(&status);

    // the object
    JSValue object = JS_NewObject(ctx);
    setSize(ctx, object, "jsHeap", heapKnown, heapKnown ? heap->used : 0);
    setSize(ctx, object, "jsHeapPeak", heapKnown, heapKnown ? heap->peak : 0);
    setSize(ctx, object, "jsHeapSize", heapLimit > 0, heapLimit);
    setSize(ctx, object, "ramFree", statusKnown, status.ramFree);
    setSize(ctx, object, "ramLargest", statusKnown, status.ramLargest);
    setSize(ctx, object, "ramLowest", statusKnown, status.ramLowest);
    setSize(ctx, object, "psramFree", statusKnown && status.psramFree > 0, status.psramFree);
    return js::Value(ctx, object);
}

/**
 * board.load(): the load of the JS thread (see rt::LoadMeter): of the last
 * second and the most over a full second, 0 to 1; the ms worked and the ms
 * elapsed since the program started, to compute it over any time.
 */
js::Value loadReport() {
    // what the loop of the program measured
    JSContext *ctx = js::context();
    rt::LoadStats stats = rt::Loop::current()->load();

    // the object
    JSValue object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, object, "js", JS_NewFloat64(ctx, stats.load));
    JS_SetPropertyStr(ctx, object, "jsPeak", JS_NewFloat64(ctx, stats.peak));
    JS_SetPropertyStr(ctx, object, "busy", JS_NewFloat64(ctx, (double)stats.busyMs));
    JS_SetPropertyStr(ctx, object, "elapsed", JS_NewFloat64(ctx, (double)stats.elapsedMs));
    return js::Value(ctx, object);
}

} // namespace

/**
 * Module "device/board": id(), mac(), restart(), memory(), load().
 */
JS_MODULE(board, module) {
    JS_FUNCTION(module, id);
    JS_FUNCTION(module, mac);
    JS_FUNCTION(module, restart);
    JS_FUNCTION_AS(module, memoryReport, "memory");   // C++ has a namespace 'memory'
    JS_FUNCTION_AS(module, loadReport, "load");
}
