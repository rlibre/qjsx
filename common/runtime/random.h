/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The random bytes of the runtime: crypto.getRandomValues(),
// crypto.randomUUID(), sys/crypto randomBytes() and the seed of Math.random()
// of each program run.
//
// They come from the platform's source (platform::randomBytes: the system on
// the desktop, the hardware on a target), unless a seed was set: the digital
// twin of a project with "twin.randomSeed" then gives a reproducible
// sequence, which starts again at each program run (each run of the program
// sees the same numbers). That sequence (xoshiro256**) is NOT for secrets.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rt {

/**
 * Fills 'buffer' with 'size' random bytes. Any thread.
 */
void randomBytes(void *buffer, size_t size);

/**
 * 64 random bits (randomBytes).
 */
uint64_t randomUint64();

/**
 * From now on, the random bytes are the reproducible sequence of 'seed'
 * (the twin, at its start, before any program runs).
 */
void setRandomSeed(uint64_t seed);

/**
 * A program run starts: the reproducible sequence, if any, starts again from
 * its seed. Nothing without a seed.
 */
void restartRandom();

} // namespace rt
