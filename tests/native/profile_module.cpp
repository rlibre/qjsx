/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Native module "profile_test": measures of the engine on the PC
// (tools/profile/). Only built into qjsx-tests.
//
//   - a sampling profiler (Windows only): start(hz) starts a thread that,
//     hz times per second, suspends the JS thread (the caller of start),
//     reads where it stopped (its instruction pointer) and counts it in a
//     table; stop() ends it and prints the counts, one line per address:
//       sample <address> <count>
//     the address in memory, after a line "base <address>" giving where the
//     executable is loaded (tools/profile/profile.sh moves the addresses to
//     the base of the file, for addr2line);
//   - empty native functions, to time each step of a call from JS: nop()
//     and the methods of the class Bench go through the binding library's
//     trampolines, rawNop() is a plain QuickJS C function.
#include <stdio.h>
#include <string.h>

#include "jsbind.h"
#include "memory.h"
#include "modules.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// ### Empty functions #########################################################

/**
 * Does nothing, through the binding library's trampoline.
 */
void nop() {
}

/**
 * Does nothing, as a plain QuickJS C function (no trampoline).
 */
JSValue rawNop(JSContext *, JSValueConst, int, JSValueConst *) {
    return JS_UNDEFINED;
}

/**
 * An object whose methods do nothing (JS class Bench), as hardware classes
 * are called (pin.toggle()).
 */
class Bench {
    JS_CLASS(Bench);

public:
    /**
     * Does nothing, no argument.
     */
    JS_EXPORT(nop) void nop() {
    }

    /**
     * Two integers converted, their sum converted back.
     */
    JS_EXPORT(add) int add(int a, int b) {
        return a + b;
    }
};

// ### Sampling profiler #######################################################

#ifdef _WIN32

const int TABLE_SIZE = 1 << 16;   // addresses the table can hold (power of two)

// One counted address (the table: open addressing, linear probing).
struct Sample {
    uint64_t address;   // 0: empty entry
    uint32_t count;     // times the JS thread was found there
};

Sample *table = nullptr;          // TABLE_SIZE entries, owned; nullptr when stopped
uint64_t lost = 0;                // samples that found the table full
uint64_t total = 0;               // samples taken
HANDLE jsThread = nullptr;        // the thread sampled (the caller of start)
HANDLE samplerThread = nullptr;   // the sampling thread
volatile LONG running = 0;        // 1 while the sampler must go on
int periodUs = 1000;              // between two samples

/**
 * Counts one address.
 */
void count(uint64_t address) {
    total++;

    // linear probing from the hash of the address
    uint32_t index = (uint32_t)((address >> 1) * 2654435761u) & (TABLE_SIZE - 1);
    for (int probe = 0; probe < TABLE_SIZE; probe++) {
        Sample *entry = &table[(index + probe) & (TABLE_SIZE - 1)];
        if (entry->address == address) {
            entry->count++;
            return;
        }

        if (entry->address == 0) {
            entry->address = address;
            entry->count = 1;
            return;
        }
    }

    lost++;
}

/**
 * The sampling thread: suspend the JS thread, read its instruction pointer,
 * resume it; then wait for the next period (high resolution timer).
 */
DWORD WINAPI sampler(void *) {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    while (running) {
        // one sample
        if (SuspendThread(jsThread) != (DWORD)-1) {
            CONTEXT context;
            memset(&context, 0, sizeof(context));
            context.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(jsThread, &context)) {
                count(context.Rip);
            }

            ResumeThread(jsThread);
        }

        // the next one (100 ns units, negative: relative)
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)periodUs * 10;
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
    }

    CloseHandle(timer);
    return 0;
}

/**
 * Starts sampling the calling (JS) thread 'hz' times per second.
 */
void start(js::Range<int, 1, 100000> hz) {
    if (table != nullptr) {
        return (void)js::fail("profile_test: already started");
    }

    // an empty table
    table = (Sample *)memory::allocate(sizeof(Sample) * TABLE_SIZE);
    memset(table, 0, sizeof(Sample) * TABLE_SIZE);
    lost = 0;
    total = 0;
    periodUs = 1000000 / (int)hz;

    // the JS thread, then the sampler
    DWORD rights = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &jsThread, rights, FALSE, 0);
    running = 1;
    samplerThread = CreateThread(nullptr, 0, &sampler, nullptr, 0, nullptr);
}

/**
 * Stops sampling, prints where the executable is loaded ("base
 * <address>"), every counted address ("sample <address> <count>"), then a
 * line "samples <total> <lost>". Returns the number of samples.
 */
double stop() {
    if (table == nullptr) {
        return js::fail("profile_test: not started");
    }

    // the sampler ends
    InterlockedExchange(&running, 0);
    WaitForSingleObject(samplerThread, INFINITE);
    CloseHandle(samplerThread);
    CloseHandle(jsThread);

    // where the executable is loaded (the loader rewrites the image base of
    // the header in memory: the base of the file is read by the script)
    uintptr_t loadedBase = (uintptr_t)GetModuleHandleW(nullptr);
    printf("base %llx\n", (unsigned long long)loadedBase);

    // the counts
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (table[i].address != 0) {
            printf("sample %llx %u\n", (unsigned long long)table[i].address, table[i].count);
        }
    }

    printf("samples %llu %llu\n", (unsigned long long)total, (unsigned long long)lost);
    fflush(stdout);

    memory::release(table);
    table = nullptr;
    return (double)total;
}

#else

/**
 * Not available: the profiler suspends threads the Windows way.
 */
void start(js::Range<int, 1, 100000>) {
    return (void)js::fail("profile_test: the profiler only runs on Windows");
}

/**
 * Not available (see start).
 */
double stop() {
    return js::fail("profile_test: the profiler only runs on Windows");
}

#endif

} // namespace

/**
 * Module "profile_test": the profiler, the empty functions, the class Bench.
 */
JS_MODULE(profile_test, module) {
    JS_FUNCTION(module, start);
    JS_FUNCTION(module, stop);
    JS_FUNCTION(module, nop);
    JS_ADD_CLASS(module, Bench);

    // the plain C function (no trampoline)
    JSContext *ctx = module.context();
    js::Value raw(ctx, JS_NewCFunction(ctx, &rawNop, "rawNop", 0));
    module.constant("rawNop", raw);
}

/**
 * Registers the module "profile_test"; called by registerTestModules().
 */
void registerProfileTestModule() {
    rt::registerNativeModule("profile_test", &jsNewModule_profile_test);
}
