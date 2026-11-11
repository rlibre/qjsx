/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

namespace memory {

namespace {

/**
 * Stops the program: nothing sensible can go on without this block.
 */
[[noreturn]] void outOfMemory(size_t size) {
    char message[64];
    snprintf(message, sizeof(message), "out of memory (%zu bytes)", size);
    platform::fatal(message);
}

} // namespace

/**
 * malloc() of at least one byte; out of memory is fatal.
 */
void *allocate(size_t size) {
    // malloc(0) may give nullptr: ask for at least one byte
    size_t asked = size > 0 ? size : 1;
    void *block = malloc(asked);
    if (block == nullptr) {
        outOfMemory(asked);
    }

    return block;
}

/**
 * realloc() to at least one byte; out of memory is fatal.
 */
void *reallocate(void *block, size_t size) {
    // same as allocate() for a size of 0; on failure the old block would
    // still be valid, but the program stops anyway
    size_t asked = size > 0 ? size : 1;
    void *resized = realloc(block, asked);
    if (resized == nullptr) {
        outOfMemory(asked);
    }

    return resized;
}

/**
 * free().
 */
void release(void *block) {
    free(block);
}

/**
 * Copies the text into a new block and adds the terminating zero.
 */
char *copyText(const char *text, size_t length) {
    char *copy = (char *)allocate(length + 1);
    memcpy(copy, text, length);
    copy[length] = 0;
    return copy;
}

} // namespace memory
