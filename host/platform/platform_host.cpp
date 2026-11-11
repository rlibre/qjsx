/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Platform layer for the desktop: Windows (Win32) and POSIX (Linux, macOS).
#include "platform.h"
#include "memory.h"

#include <stdio.h>
#include <stdlib.h>

namespace platform {

/**
 * Desktop: prints the message on stderr and aborts (a debugger or the
 * system catches it).
 */
void fatal(const char *message) {
    fprintf(stderr, "qjsx: fatal: %s\n", message);
    fflush(stderr);
    abort();
}

/**
 * Desktop: nothing to do, the system preempts the JS thread by itself.
 */
void yieldIfDue() {
}

} // namespace platform

#if defined(_WIN32)

#include <windows.h>

namespace platform {

/**
 * Milliseconds since the system started (GetTickCount64): never goes back.
 */
int64_t monotonicMs() {
    return (int64_t)GetTickCount64();
}

// What Signal::_state points to.
struct SignalState {
    CRITICAL_SECTION lock;          // taken by lock() / unlock()
    CONDITION_VARIABLE condition;   // waited on by wait(), woken by notify()
};

/**
 * Allocates and initializes the lock and the condition variable (freed by
 * the destructor).
 */
Signal::Signal() {
    SignalState *state = (SignalState *)memory::allocate(sizeof(SignalState));
    InitializeCriticalSection(&state->lock);
    InitializeConditionVariable(&state->condition);
    _state = state;
}

/**
 * Deletes the lock and frees the state (a Win32 condition variable needs
 * no cleanup).
 */
Signal::~Signal() {
    SignalState *state = (SignalState *)_state;
    DeleteCriticalSection(&state->lock);
    memory::release(state);
}

/**
 * Takes the lock.
 */
void Signal::lock() {
    EnterCriticalSection(&((SignalState *)_state)->lock);
}

/**
 * Releases the lock.
 */
void Signal::unlock() {
    LeaveCriticalSection(&((SignalState *)_state)->lock);
}

/**
 * Waits for notify() at most timeoutMs (< 0: forever), with the lock held (released
 * while waiting). Returns false on timeout (or failure).
 */
bool Signal::wait(int64_t timeoutMs) {
    SignalState *state = (SignalState *)_state;
    DWORD timeout = timeoutMs < 0 ? INFINITE : (DWORD)timeoutMs;
    return SleepConditionVariableCS(&state->condition, &state->lock, timeout) != 0;
}

/**
 * Wakes one thread waiting in wait().
 */
void Signal::notify() {
    WakeConditionVariable(&((SignalState *)_state)->condition);
}

namespace {
// What startThread() gives to the new thread (malloc'd, freed by the thread).
struct ThreadStart {
    ThreadFunction *run;   // thread function
    void *argument;        // passed to run
};

/**
 * Entry of the threads made by startThread(): runs the thread function.
 */
DWORD WINAPI threadEntry(LPVOID parameter) {
    // the start record belongs to this thread: copy it, free it
    ThreadStart start = *(ThreadStart *)parameter;
    memory::release(parameter);

    // run the thread function
    start.run(start.argument);
    return 0;
}
} // namespace

/**
 * Blocks the calling thread for 'milliseconds'.
 */
void sleepMs(int64_t milliseconds) {
    Sleep((DWORD)milliseconds);
}

/**
 * Starts a detached thread running run(argument). False if it cannot start.
 */
bool startThread(ThreadFunction *run, void *argument, ThreadOptions options) {
    // the start record is freed by the new thread (or below if it cannot start)
    ThreadStart *start = (ThreadStart *)memory::allocate(sizeof(ThreadStart));
    *start = ThreadStart{ run, argument };

    // create the thread: only the stack size is used (0, DEFAULT_STACK, is the
    // system default), priorities and cores are left to the OS
    HANDLE thread = CreateThread(nullptr, options.stackSize, &threadEntry, start, 0, nullptr);
    if (thread == nullptr) {
        memory::release(start);
        return false;
    }

    CloseHandle(thread);   // detached
    return true;
}

/**
 * Allocates and initializes the critical section (freed by the destructor).
 */
Mutex::Mutex() {
    CRITICAL_SECTION *section = (CRITICAL_SECTION *)memory::allocate(sizeof(CRITICAL_SECTION));
    InitializeCriticalSection(section);
    _state = section;
}

/**
 * Deletes and frees the critical section.
 */
Mutex::~Mutex() {
    DeleteCriticalSection((CRITICAL_SECTION *)_state);
    memory::release(_state);
}

/**
 * Takes the lock (recursive on Windows: a critical section can be entered
 * again by its owner).
 */
void Mutex::lock() {
    EnterCriticalSection((CRITICAL_SECTION *)_state);
}

/**
 * Releases the lock.
 */
void Mutex::unlock() {
    LeaveCriticalSection((CRITICAL_SECTION *)_state);
}

} // namespace platform

#else

#include <pthread.h>
#include <time.h>
#include <errno.h>

namespace platform {

/**
 * Milliseconds of CLOCK_MONOTONIC: never goes back.
 */
int64_t monotonicMs() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

// What Signal::_state points to.
struct SignalState {
    pthread_mutex_t lock;       // taken by lock() / unlock()
    pthread_cond_t condition;   // waited on by wait(), woken by notify()
};

/**
 * Allocates and initializes the mutex and the condition variable (freed by
 * the destructor).
 */
Signal::Signal() {
    SignalState *state = (SignalState *)memory::allocate(sizeof(SignalState));
    pthread_mutex_init(&state->lock, nullptr);

    // Linux: the condition measures its deadlines on CLOCK_MONOTONIC, so that
    // a change of the wall clock does not shorten or stretch wait(); macOS has
    // no pthread_condattr_setclock: wait() uses a relative timeout there
    pthread_condattr_t attributes;
    pthread_condattr_init(&attributes);
#if !defined(__APPLE__)
    pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&state->condition, &attributes);
    pthread_condattr_destroy(&attributes);
    _state = state;
}

/**
 * Destroys the condition variable and the mutex, frees the state.
 */
Signal::~Signal() {
    SignalState *state = (SignalState *)_state;
    pthread_cond_destroy(&state->condition);
    pthread_mutex_destroy(&state->lock);
    memory::release(state);
}

/**
 * Takes the lock.
 */
void Signal::lock() {
    pthread_mutex_lock(&((SignalState *)_state)->lock);
}

/**
 * Releases the lock.
 */
void Signal::unlock() {
    pthread_mutex_unlock(&((SignalState *)_state)->lock);
}

/**
 * Waits for notify() at most timeoutMs (< 0: forever), with the lock held (released
 * while waiting). Returns false on timeout.
 */
bool Signal::wait(int64_t timeoutMs) {
    // no timeout: wait until notified
    SignalState *state = (SignalState *)_state;
    if (timeoutMs < 0) {
        pthread_cond_wait(&state->condition, &state->lock);
        return true;
    }

#if defined(__APPLE__)
    // macOS: the timeout is given as a duration, which no clock change affects
    struct timespec timeout;
    timeout.tv_sec = (time_t)(timeoutMs / 1000);
    timeout.tv_nsec = (long)(timeoutMs % 1000) * 1000000;
    return pthread_cond_timedwait_relative_np(&state->condition, &state->lock, &timeout) != ETIMEDOUT;
#else
    // elsewhere pthread_cond_timedwait takes an absolute deadline on the
    // condition's clock (CLOCK_MONOTONIC, see the constructor): add the
    // timeout to now, nanoseconds carried into seconds
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeoutMs / 1000;
    deadline.tv_nsec += (timeoutMs % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000;
    }

    // wait until notified or past the deadline
    return pthread_cond_timedwait(&state->condition, &state->lock, &deadline) != ETIMEDOUT;
#endif
}

/**
 * Wakes one thread waiting in wait().
 */
void Signal::notify() {
    pthread_cond_signal(&((SignalState *)_state)->condition);
}

namespace {
// What startThread() gives to the new thread (malloc'd, freed by the thread).
struct ThreadStart {
    ThreadFunction *run;   // thread function
    void *argument;        // passed to run
};

/**
 * Entry of the threads made by startThread(): runs the thread function.
 */
void *threadEntry(void *parameter) {
    // the start record belongs to this thread: copy it, free it
    ThreadStart start = *(ThreadStart *)parameter;
    memory::release(parameter);

    // run the thread function
    start.run(start.argument);
    return nullptr;
}
} // namespace

/**
 * Blocks the calling thread for 'milliseconds'.
 */
void sleepMs(int64_t milliseconds) {
    struct timespec delay;
    delay.tv_sec = (time_t)(milliseconds / 1000);
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000;
    nanosleep(&delay, nullptr);
}

/**
 * Starts a detached thread running run(argument). False if it cannot start.
 */
bool startThread(ThreadFunction *run, void *argument, ThreadOptions options) {
    // the start record is freed by the new thread (or below if it cannot start)
    ThreadStart *start = (ThreadStart *)memory::allocate(sizeof(ThreadStart));
    *start = ThreadStart{ run, argument };

    // only the stack size is used (DEFAULT_STACK keeps the system default),
    // priorities and cores are left to the OS
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    if (options.stackSize != DEFAULT_STACK) {
        pthread_attr_setstacksize(&attributes, options.stackSize);
    }

    // create the thread; the attributes are no longer needed
    pthread_t thread;
    int failed = pthread_create(&thread, &attributes, &threadEntry, start);
    pthread_attr_destroy(&attributes);
    if (failed != 0) {
        memory::release(start);
        return false;
    }

    // nobody joins it
    pthread_detach(thread);
    return true;
}

/**
 * Allocates and initializes the pthread mutex, recursive like the Windows
 * critical section (freed by the destructor).
 */
Mutex::Mutex() {
    // a recursive mutex: its owner may take it again (one unlock() per lock())
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);

    // create it; the attributes are no longer needed
    pthread_mutex_t *mutex = (pthread_mutex_t *)memory::allocate(sizeof(pthread_mutex_t));
    pthread_mutex_init(mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
    _state = mutex;
}

/**
 * Destroys and frees the pthread mutex.
 */
Mutex::~Mutex() {
    pthread_mutex_destroy((pthread_mutex_t *)_state);
    memory::release(_state);
}

/**
 * Takes the lock (recursive: the owner may take it again).
 */
void Mutex::lock() {
    pthread_mutex_lock((pthread_mutex_t *)_state);
}

/**
 * Releases the lock.
 */
void Mutex::unlock() {
    pthread_mutex_unlock((pthread_mutex_t *)_state);
}

} // namespace platform

#endif
