/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Platform layer of the runtime: the few system services the event loop
// needs. One implementation per platform (platform_host.cpp for Windows,
// macOS and Linux; the ESP32 one will use FreeRTOS).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace platform {

/**
 * Milliseconds from an arbitrary origin, never going backwards. Signed on
 * purpose: deadlines minus now give negative delays for expired timers. On
 * 64 bits it never wraps (292 million years); a port must use a 64-bit
 * source (esp_timer_get_time, time_us_64), never the 32-bit FreeRTOS tick
 * count, which wraps after 49 days at 1 kHz.
 */
int64_t monotonicMs();

/**
 * Stops the program after printing 'message' on the console: abort on the
 * desktop, restart of the chip on a target. For what nothing can go on after
 * (out of memory).
 */
[[noreturn]] void fatal(const char *message);

/**
 * Blocks the calling thread (never the JS thread while JS code waits).
 */
void sleepMs(int64_t milliseconds);

/**
 * Called often by the JS thread while JS code runs (interrupt handler of the
 * runtime): from time to time, blocks it for one tick, so that the
 * lower-priority tasks of its core run. Without it, JS code that computes for
 * a long time starves the idle task, and the task watchdog of the RTOS
 * complains. JS thread only. Does nothing on the desktop.
 */
void yieldIfDue();

// How a thread is started. On an RTOS every field counts; the desktop only
// uses the stack size.
struct ThreadOptions {
    const char *name;        // shown by the RTOS tools
    size_t      stackSize;   // bytes; DEFAULT_STACK: the platform's default
    int         priority;    // DEFAULT_PRIORITY, or higher numbers for more urgent threads
    int         core;        // ANY_CORE, or the core the thread is pinned to (ESP32, RP2350)
};

const size_t DEFAULT_STACK = 0;
const int DEFAULT_PRIORITY = 0;
const int ANY_CORE = -1;

/**
 * Options of a thread with the platform's defaults.
 */
inline ThreadOptions defaultThreadOptions(const char *name) {
    return ThreadOptions{ name, DEFAULT_STACK, DEFAULT_PRIORITY, ANY_CORE };
}

// Body of a thread started by startThread().
typedef void ThreadFunction(void *argument);

/**
 * Starts a detached thread running run(argument) (nobody joins it: the
 * thread ends when run returns). Returns false on failure; 'argument' is
 * then still owned by the caller.
 */
bool startThread(ThreadFunction *run, void *argument, ThreadOptions options);

/**
 * Atomic reference counting (gcc / clang builtins): adds 1, returns the new
 * value.
 */
inline int atomicIncrement(int *value) {
    return __atomic_add_fetch(value, 1, __ATOMIC_ACQ_REL);
}

/**
 * Subtracts 1, returns the new value (0: the last reference is gone).
 */
inline int atomicDecrement(int *value) {
    return __atomic_sub_fetch(value, 1, __ATOMIC_ACQ_REL);
}

// Mutex alone (no condition). Recursive on every platform: the thread that
// holds it may lock it again, and must unlock it as many times (Windows
// critical section, POSIX PTHREAD_MUTEX_RECURSIVE, FreeRTOS recursive mutex).
class Mutex {
public:
    /**
     * Creates the system lock (unlocked, recursive).
     */
    Mutex();

    /**
     * Destroys the system lock: must not be held.
     */
    ~Mutex();
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;

    /**
     * Blocks until the calling thread holds the lock; returns at once if it
     * already holds it (recursive).
     */
    void  lock();

    /**
     * Releases one lock() of the calling thread.
     */
    void  unlock();

private:
    void *_state;  // system lock (platform-specific); owned
};

// Mutex + condition: the event queue is filled by producer threads (or
// interrupts on the ESP) and emptied by the JS thread.
class Signal {
public:
    /**
     * Creates the lock (unlocked) and the condition.
     */
    Signal();

    /**
     * Destroys the lock and the condition: nobody may hold or wait on them.
     */
    ~Signal();
    Signal(const Signal &) = delete;
    Signal &operator=(const Signal &) = delete;

    /**
     * Blocks until the calling thread holds the lock.
     */
    void  lock();

    /**
     * Releases the lock held by the calling thread.
     */
    void  unlock();

    /**
     * Called with the lock held; releases it while waiting. timeoutMs < 0
     * waits forever. Returns false on timeout. May return early (spurious
     * wake up): the caller checks its own condition again.
     */
    bool  wait(int64_t timeoutMs);

    /**
     * Wakes the waiting thread (lock held or not).
     */
    void  notify();

private:
    void *_state;  // system lock + condition (platform-specific); owned
};

} // namespace platform
