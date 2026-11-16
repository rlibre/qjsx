/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The random bytes of the runtime (see random.h): the platform's source, or
// the reproducible sequence of a seed (xoshiro256**, its state made from the
// seed by splitmix64, as its authors recommend).
#include "random.h"
#include "platform.h"

#include <string.h>

namespace {

// The reproducible sequence (all under 'lock').
struct Sequence {
    bool     seeded;     // a seed was set: the bytes come from 'state'
    uint64_t seed;       // the seed, to start again at each program run
    uint64_t state[4];   // xoshiro256** state; never all zero
};

Sequence sequence;          // lock
platform::Mutex lock;       // sequence

/**
 * splitmix64: the next value of '*value' (the state of xoshiro256** is made
 * of four of them).
 */
uint64_t splitmix64(uint64_t *value) {
    *value += 0x9E3779B97F4A7C15u;
    uint64_t mixed = *value;
    mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9u;
    mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBu;
    return mixed ^ (mixed >> 31);
}

/**
 * 64-bit left rotation (0 < count < 64).
 */
uint64_t rotateLeft(uint64_t value, int count) {
    return (value << count) | (value >> (64 - count));
}

/**
 * xoshiro256**: the next 64 bits of the sequence (under 'lock').
 */
uint64_t nextOfSequence() {
    uint64_t *state = sequence.state;
    uint64_t result = rotateLeft(state[1] * 5, 7) * 9;
    uint64_t shifted = state[1] << 17;

    state[2] ^= state[0];
    state[3] ^= state[1];
    state[1] ^= state[2];
    state[0] ^= state[3];
    state[2] ^= shifted;
    state[3] = rotateLeft(state[3], 45);
    return result;
}

/**
 * The state of the sequence from its seed (under 'lock').
 */
void startSequence() {
    uint64_t value = sequence.seed;
    for (int index = 0; index < 4; index++) {
        sequence.state[index] = splitmix64(&value);
    }
}

} // namespace

namespace rt {

/**
 * The reproducible sequence when a seed was set, 8 bytes at a time; else the
 * platform's source.
 */
void randomBytes(void *buffer, size_t size) {
    // no seed: the platform's source
    lock.lock();
    if (!sequence.seeded) {
        lock.unlock();
        platform::randomBytes(buffer, size);
        return;
    }

    // the sequence, the last piece cut
    uint8_t *next = (uint8_t *)buffer;
    while (size > 0) {
        uint64_t bits = nextOfSequence();
        size_t piece = size < sizeof(bits) ? size : sizeof(bits);
        memcpy(next, &bits, piece);
        next += piece;
        size -= piece;
    }
    lock.unlock();
}

/**
 * 64 random bits.
 */
uint64_t randomUint64() {
    uint64_t bits = 0;
    randomBytes(&bits, sizeof(bits));
    return bits;
}

/**
 * Keeps the seed and starts the sequence.
 */
void setRandomSeed(uint64_t seed) {
    lock.lock();
    sequence.seeded = true;
    sequence.seed = seed;
    startSequence();
    lock.unlock();
}

/**
 * Starts the sequence again from its seed, if any.
 */
void restartRandom() {
    lock.lock();
    if (sequence.seeded) {
        startSequence();
    }
    lock.unlock();
}

} // namespace rt
