/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The analyzer of the pins of the digital twin (see twin.h): the changes of
// each pin with their time, for the analyzer panel of the page.
//
// Two sets of changes: the threads that change a pin record into one while
// the page's server reads the other (scopeRead swaps them), so the program
// never waits for the page to build its message.
//
// Threads: scopeRecord on any thread (with the twin's state lock held: lock
// is taken after it), scopeRead on the page's server thread only.
#include "twin.h"
#include "memory.h"
#include "platform.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace twin {

namespace {

const int MAX_PINS = 64;                 // pins the twin can have (as twin.cpp)

ScopeChanges sets[2][MAX_PINS];          // the two sets of changes, one per pin
ScopeChanges *filling = sets[0];         // lock: the set the changes go into
ScopeRecord current[MAX_PINS];           // lock: the level of each pin now
ScopeRecord currentPwm[MAX_PINS];        // lock: the last PWM change of each pin (pwmFrequency 0: none)
ScopeRecord readStates[MAX_PINS];        // the levels copied by scopeRead (its thread only)
ScopeRecord readPwm[MAX_PINS];           // the PWM changes copied by scopeRead (its thread only)
ScopeTrain trainSets[2][SCOPE_TRAINS];   // the RMT sends: one set filled, the other read
int trainCounts[2];                      // sends in each set (lock for the set filled)
int fillingTrains = 0;                   // lock: the set the sends go into
platform::Mutex lock;                    // filling, current
int64_t startUs = 0;                     // the clock at scopeStart()

/**
 * Microseconds of the system's monotonic clock.
 */
int64_t clockUs() {
#ifdef _WIN32
    // the performance counter, split in seconds and the rest so that the
    // conversion to µs cannot overflow
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    int64_t seconds = counter.QuadPart / frequency.QuadPart;
    int64_t rest = counter.QuadPart % frequency.QuadPart;
    return seconds * 1000000 + rest * 1000000 / frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
#endif
}

} // namespace

/**
 * The time 0 of the analyzer.
 */
void scopeStart() {
    startUs = clockUs();
}

/**
 * µs since scopeStart().
 */
int64_t scopeNowUs() {
    return clockUs() - startUs;
}

/**
 * The state of a pin at rest.
 */
void scopeSetInitial(int pin, int level, int millivolts) {
    if (pin < 0 || pin >= MAX_PINS) {
        return;
    }

    lock.lock();
    current[pin] = ScopeRecord{ scopeNowUs(), level, millivolts, false, 0, 0, false };
    lock.unlock();
}

namespace {

/**
 * Keeps a change of a pin in the records while there is room, else only
 * counts it. lock held.
 */
void keepChange(int pin, const ScopeRecord *change) {
    ScopeChanges *changes = &filling[pin];
    if (changes->count < SCOPE_RECORDS) {
        changes->records[changes->count] = *change;
        changes->count++;
    } else {
        changes->dropped++;
        changes->droppedUntilUs = change->timeUs;
    }
}

} // namespace

/**
 * Records a change of level: the pin's level now, and kept.
 */
void scopeRecord(int pin, int level, int millivolts) {
    if (pin < 0 || pin >= MAX_PINS) {
        return;
    }

    lock.lock();
    ScopeRecord change = { scopeNowUs(), level, millivolts, false, 0, 0, false };
    current[pin] = change;
    keepChange(pin, &change);
    lock.unlock();
}

/**
 * Records a change of the PWM output: the pin's PWM now, and kept.
 */
void scopeRecordPwm(int pin, int64_t timeUs, double frequency, double duty, bool invert) {
    if (pin < 0 || pin >= MAX_PINS) {
        return;
    }

    lock.lock();
    ScopeRecord change = { timeUs, 0, 0, true, frequency, duty, invert };
    currentPwm[pin] = change;
    keepChange(pin, &change);
    lock.unlock();
}

/**
 * 1. swaps the sets and copies the states now, under the lock;
 * 2. visits each pin, lock released (the program records into the other set);
 * 3. empties the set read, for its next turn.
 */
int64_t scopeRead(int count, ScopeVisitor *visit, void *context) {
    int pins = count < MAX_PINS ? count : MAX_PINS;

    // swap the sets, copy the states
    lock.lock();
    ScopeChanges *full = filling;
    filling = filling == sets[0] ? sets[1] : sets[0];
    for (int pin = 0; pin < pins; pin++) {
        readStates[pin] = current[pin];
        readPwm[pin] = currentPwm[pin];
    }
    int64_t nowUs = scopeNowUs();
    lock.unlock();

    // each pin
    for (int pin = 0; pin < pins; pin++) {
        visit(context, pin, &full[pin], &readStates[pin], &readPwm[pin]);
    }

    // empty for the next turn
    for (int pin = 0; pin < MAX_PINS; pin++) {
        full[pin].count = 0;
        full[pin].dropped = 0;
    }

    return nowUs;
}

/**
 * Records an RMT send: a copy of it in the set filled, while there is room.
 */
void scopeRecordTrain(const ScopeTrain *train) {
    lock.lock();
    int *count = &trainCounts[fillingTrains];
    if (*count < SCOPE_TRAINS) {
        ScopeTrain *kept = &trainSets[fillingTrains][*count];
        *kept = *train;
        kept->data = (uint8_t *)memory::allocate(train->size > 0 ? train->size : 1);
        memcpy(kept->data, train->data, train->size);
        (*count)++;
    }
    lock.unlock();
}

/**
 * 1. swaps the sets of sends, under the lock;
 * 2. visits the sends of the set read, lock released;
 * 3. frees their data, the set empty for its next turn.
 */
void scopeReadTrains(TrainVisitor *visit, void *context) {
    // swap the sets
    lock.lock();
    int full = fillingTrains;
    fillingTrains = full == 0 ? 1 : 0;
    lock.unlock();

    // each send, then its data freed
    for (int i = 0; i < trainCounts[full]; i++) {
        ScopeTrain *train = &trainSets[full][i];
        visit(context, train);
        memory::release(train->data);
    }
    trainCounts[full] = 0;
}

} // namespace twin
