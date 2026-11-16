/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The sampling profiler of a device (compiled with QJSX_PROFILER: on by
// default on the PC, the desktop agent and the twin; idf.py
// -DQJSX_PROFILER=ON for a firmware). "qjsx profile" drives it through the
// agent (PROFILE_START, PROFILE_STOP).
//
// How it samples:
// 1. a thread of its own wakes every interval (1 ms by default). If the JS
//    thread waits in its loop (rt::JsLoadUsage::working), the sample is
//    "idle"; else it asks the interpreter for an interrupt
//    (JS_RequestInterrupt);
// 2. at its next check, on the JS thread, the interpreter calls the
//    runtime's interrupt handler, which calls sample(): the stack of JS
//    frames, as (module, function number) pairs, innermost first, MAX_DEPTH
//    at most, is counted in a table of distinct stacks;
// 3. stop() hands the table over; the host turns module and function numbers
//    into names with the debug files of the package.
// The table (about 5 KB) is allocated by start() and freed by release():
// nothing is taken while no profile runs.
#pragma once

#ifdef QJSX_PROFILER

#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"
#include "loop.h"

namespace agent {
namespace profiler {

const int MAX_DEPTH = 8;      // frames kept per sample, innermost first
const int MAX_STACKS = 128;   // distinct stacks kept
const int MAX_MODULES = 32;   // distinct modules kept

// A frame of a sample: (index of its module << 16) | its function number.
// The module index NO_MODULE: a module the table had no room for.
const uint32_t NO_MODULE = 0xFFFF;

// A distinct stack and how many samples had it.
struct Stack {
    uint32_t count;               // samples with this stack
    int      depth;               // frames used
    uint32_t frames[MAX_DEPTH];   // innermost first
};

// What stop() hands over; valid until release().
struct Report {
    int64_t            durationMs;    // from start() to stop()
    int                intervalMs;    // asked to start()
    uint32_t           samples;       // samples taken while the JS thread worked
    uint32_t           idle;          // samples while it waited in its loop
    uint32_t           overflow;      // samples whose stack had no room in the table
    const Stack       *stacks;        // stackCount entries; borrowed
    int                stackCount;    // distinct stacks
    const char *const *modules;       // module names, by index; borrowed
    int                moduleCount;   // modules
};

/**
 * Starts a profile, sampling every 'intervalMs' (1 to 1000): allocates the
 * table (a profile in progress starts again). Any thread (the agent's).
 */
void start(int intervalMs);

/**
 * Stops sampling and fills *out with the profile, valid until release().
 * Without start(): an empty report. Any thread.
 */
void stop(Report *out);

/**
 * Frees what the last profile allocated. Any thread.
 */
void release();

/**
 * A run starts (JS thread): its context, and where its loop publishes
 * whether it works. Both stay valid until detach().
 */
void attach(JSContext *ctx, const rt::JsLoadUsage *load);

/**
 * The run ends (JS thread, before its context is freed).
 */
void detach();

/**
 * From the interrupt handler of the runtime (JS thread): takes the sample
 * asked for, if any.
 */
void sample(JSContext *ctx);

} // namespace profiler
} // namespace agent

#endif // QJSX_PROFILER
