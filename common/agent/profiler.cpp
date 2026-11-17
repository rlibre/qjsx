/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The sampling profiler of a device: see profiler.h.
//
// Threads: start(), stop(), release() on the agent's thread; attach(),
// detach(), sample() on the JS thread; the sampling thread of its own. All
// of them hold 'lock' while they touch the state, but for 'pending', which
// the sampling thread sets and sample() takes (atomic).
#include "profiler.h"

#ifdef QJSX_PROFILER

#include <string.h>

#include "memory.h"
#include "platform.h"
#include "quickjs-debug.h"

namespace agent {
namespace profiler {

namespace {

const int IDLE_SLEEP_MS = 50;      // the sampling thread between two profiles
const int LONGEST_INTERVAL = 1000; // ms between two samples, at most

// The state of the profiler, under lock() but for 'pending'.
struct State {
    bool                   running;                 // a profile is in progress
    int                    intervalMs;              // between two samples
    int64_t                startMs;                 // when the profile started (monotonicMs)
    JSContext             *ctx;                     // the run's context; nullptr between runs
    const rt::JsLoadUsage *load;                    // where the run's loop says it works; borrowed
    Stack                 *stacks;                  // MAX_STACKS entries, allocated by start(); nullptr: none
    int                    stackCount;              // entries used
    char                  *modules[MAX_MODULES];    // module names (memory::copyText)
    JSAtom                 atoms[MAX_MODULES];      // their atoms in the current run; JS_ATOM_NULL: not seen yet
    int                    moduleCount;             // modules known
    uint32_t               samples;                 // samples taken while the JS thread worked
    uint32_t               idle;                    // samples while it waited
    uint32_t               overflow;                // samples without room in the table
    bool                   threadStarted;           // the sampling thread runs (for good)
    int                    pending;                 // a sample asked for, not taken yet (atomic)
};

State state = {};

/**
 * The lock of the state (made on first use).
 */
platform::Mutex *lock() {
    static platform::Mutex mutex;
    return &mutex;
}

/**
 * Frees the table and the module names (lock held).
 */
void freeTable() {
    memory::release(state.stacks);
    state.stacks = nullptr;
    state.stackCount = 0;

    for (int i = 0; i < state.moduleCount; i++) {
        memory::release(state.modules[i]);
        state.modules[i] = nullptr;
        state.atoms[i] = JS_ATOM_NULL;
    }

    state.moduleCount = 0;
}

/**
 * One tick of the sampling thread (lock held): an idle sample when the JS
 * thread waits, else a sample asked for at its next check. Nothing between
 * two runs.
 */
void askSample() {
    // between two runs: nothing to sample
    if (state.ctx == nullptr || state.load == nullptr) {
        return;
    }

    // the JS thread waits in its loop: an idle sample
    bool working = __atomic_load_n(&state.load->working, __ATOMIC_RELAXED) != 0;
    if (!working) {
        state.idle++;
        return;
    }

    // else the interpreter takes the sample at its next check (sample())
    __atomic_store_n(&state.pending, 1, __ATOMIC_RELAXED);
    JS_RequestInterrupt(state.ctx);
}

/**
 * The sampling thread: one tick per interval while a profile runs, a long
 * sleep otherwise. Started once, runs for good.
 */
void samplingThread(void *) {
    for (;;) {
        // a long sleep, unless a profile runs: then one tick, and its interval
        int sleep = IDLE_SLEEP_MS;

        lock()->lock();
        if (state.running) {
            sleep = state.intervalMs;
            askSample();
        }

        lock()->unlock();

        platform::sleepMs(sleep);
    }
}

/**
 * Index of the module named by 'atom' in the table, added on first sight;
 * NO_MODULE when it is unknown or there is no room (JS thread, lock held).
 */
uint32_t moduleIndex(JSContext *ctx, JSAtom atom) {
    if (atom == JS_ATOM_NULL) {
        return NO_MODULE;
    }

    // seen already in this run
    for (int i = 0; i < state.moduleCount; i++) {
        if (state.atoms[i] == atom) {
            return (uint32_t)i;
        }
    }

    // its name (a failure leaves an exception: dropped)
    const char *name = JS_AtomToCString(ctx, atom);
    if (name == nullptr) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return NO_MODULE;
    }

    // known by its name (seen in an earlier run), or added
    uint32_t index = NO_MODULE;

    for (int i = 0; i < state.moduleCount && index == NO_MODULE; i++) {
        if (strcmp(state.modules[i], name) == 0) {
            index = (uint32_t)i;
        }
    }

    if (index == NO_MODULE && state.moduleCount < MAX_MODULES) {
        index = (uint32_t)state.moduleCount;
        state.modules[index] = memory::copyText(name, strlen(name));
        state.moduleCount++;
    }

    if (index != NO_MODULE) {
        state.atoms[index] = atom;
    }

    JS_FreeCString(ctx, name);
    return index;
}

/**
 * Counts the stack of JS frames now in the table (JS thread, lock held).
 */
void record(JSContext *ctx) {
    // the bytecode frames, innermost first (native ones skipped)
    JSDebugSampleFrame running[MAX_DEPTH];
    int depth = JS_DebugSampleStack(ctx, running, MAX_DEPTH);

    // each frame packed as (module index << 16) | function number
    Stack sampled = {};

    for (int level = 0; level < depth; level++) {
        uint32_t module = moduleIndex(ctx, running[level].module_name);
        uint32_t function = (uint32_t)running[level].function_number & 0xFFFF;
        sampled.frames[level] = (module << 16) | function;
    }

    sampled.depth = depth;
    state.samples++;

    // the same stack seen before: one more
    for (int i = 0; i < state.stackCount; i++) {
        Stack *known = &state.stacks[i];
        bool same = known->depth == sampled.depth
            && memcmp(known->frames, sampled.frames, sampled.depth * sizeof(uint32_t)) == 0;
        if (same) {
            known->count++;
            return;
        }
    }

    // else a new entry, if there is room
    if (state.stackCount == MAX_STACKS) {
        state.overflow++;
        return;
    }

    sampled.count = 1;
    state.stacks[state.stackCount] = sampled;
    state.stackCount++;
}

} // namespace

/**
 * A new table, counters at 0, the sampling thread started the first time.
 */
void start(int intervalMs) {
    lock()->lock();

    // the table of this profile (a profile in progress starts again)
    freeTable();
    state.stacks = (Stack *)memory::allocate(MAX_STACKS * sizeof(Stack));
    state.samples = 0;
    state.idle = 0;
    state.overflow = 0;
    __atomic_store_n(&state.pending, 0, __ATOMIC_RELAXED);

    // the interval, brought into 1..LONGEST_INTERVAL
    if (intervalMs < 1) {
        state.intervalMs = 1;
    } else if (intervalMs > LONGEST_INTERVAL) {
        state.intervalMs = LONGEST_INTERVAL;
    } else {
        state.intervalMs = intervalMs;
    }

    // then sampling, by a thread started the first time
    state.startMs = platform::monotonicMs();
    state.running = true;

    if (!state.threadStarted) {
        state.threadStarted = platform::startThread(&samplingThread, nullptr, platform::defaultThreadOptions("profiler"));
    }

    lock()->unlock();
}

/**
 * Stops sampling; the report points into the table until release().
 */
void stop(Report *out) {
    lock()->lock();

    // no more samples; the duration of the profile (0 without start())
    if (state.running) {
        state.running = false;
        out->durationMs = platform::monotonicMs() - state.startMs;
    } else {
        out->durationMs = 0;
    }

    // the counters, and the table itself (not copied)
    out->intervalMs = state.intervalMs;
    out->samples = state.samples;
    out->idle = state.idle;
    out->overflow = state.overflow;
    out->stacks = state.stacks;
    out->stackCount = state.stackCount;
    out->modules = state.modules;
    out->moduleCount = state.moduleCount;

    lock()->unlock();
}

/**
 * Frees the table of the last profile.
 */
void release() {
    lock()->lock();
    freeTable();
    lock()->unlock();
}

/**
 * The run's context and load; the atoms of an earlier run mean nothing in
 * this one (the names stay).
 */
void attach(JSContext *ctx, const rt::JsLoadUsage *load) {
    lock()->lock();
    state.ctx = ctx;
    state.load = load;

    for (int i = 0; i < state.moduleCount; i++) {
        state.atoms[i] = JS_ATOM_NULL;
    }

    lock()->unlock();
}

/**
 * No run any more: no sample is asked for.
 */
void detach() {
    lock()->lock();
    state.ctx = nullptr;
    state.load = nullptr;
    __atomic_store_n(&state.pending, 0, __ATOMIC_RELAXED);
    lock()->unlock();
}

/**
 * Takes the sample asked for, if any, while a profile runs.
 */
void sample(JSContext *ctx) {
    // no sample asked for (the usual case: no lock taken)
    bool asked = __atomic_exchange_n(&state.pending, 0, __ATOMIC_RELAXED) != 0;
    if (!asked) {
        return;
    }

    // recorded only for the run being profiled
    lock()->lock();
    bool profiled = state.running && state.stacks != nullptr && state.ctx == ctx;
    if (profiled) {
        record(ctx);
    }

    lock()->unlock();
}

} // namespace profiler
} // namespace agent

#endif // QJSX_PROFILER
