/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Memory of our C++ code (not the JS heap of QuickJS, which has its own
// allocator and turns a failure into a JS exception).
//
// Running out of memory is fatal: allocate() and reallocate() never return
// nullptr. They print "out of memory" and stop the program (abort on the
// desktop, restart of the chip on a target): none of our callers could
// recover in the middle of a protocol message or an I/O.
//
// Being the single entry point of our allocations, this is also where they
// can be counted or traced (profiling) without touching the rest of the code.
//
// The blocks come from the C heap: release() is free(), and a block of the C
// library (getcwd...) may be released with it.
#pragma once

#include <stddef.h>

namespace memory {

/**
 * A new block of 'size' bytes (at least 1). Never nullptr.
 */
void *allocate(size_t size);

/**
 * A new block of 'size' bytes (at least 1), or nullptr when there is no room:
 * the only allocation that may fail, for a block whose size comes from
 * outside and may simply be too large (a package received in RAM). Released
 * with release().
 */
void *tryAllocate(size_t size);

/**
 * Resizes a block (nullptr: a new one), keeping its contents. Never nullptr.
 */
void *reallocate(void *block, size_t size);

/**
 * Frees a block (nullptr: nothing).
 */
void  release(void *block);

/**
 * A zero-terminated copy of 'length' bytes of 'text'. Never nullptr.
 */
char *copyText(const char *text, size_t length);

} // namespace memory
