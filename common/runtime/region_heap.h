/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A heap in one region of raw memory that no C library manages: the PSRAM of
// the RP2350 (its JS heap), later the AXI SRAM of an STM32H7. Portable, also
// built on the desktop (tests, and the QJSX_JS_HEAP_MB option of the desktop
// board).
//
// Two-level segregated free lists (the idea of TLSF, written for this
// project):
//   - each block starts with an 8-byte header (size and flags, a check word);
//     its payload is 8-byte aligned;
//   - a free block also keeps its size in its last word, and the block after
//     it knows it follows a free block: freeing merges with both neighbours
//     in constant time, so two free blocks are never next to each other;
//   - free blocks are kept in lists by size class: one class every 8 bytes up
//     to 256 bytes, then each power of two split in four. A bitmap of the
//     non-empty lists finds the first list that can serve a request in
//     constant time;
//   - reallocate() grows a block in place when the next one is free and big
//     enough, and shrinks it in place.
//
// Running out of memory returns nullptr (QuickJS then throws its "out of
// memory" error): unlike memory::allocate(), not fatal. Releasing a block
// the heap does not own, or twice, is fatal.
//
// Not thread-safe: one thread uses a heap (the JS thread on a device), or
// the caller locks (jsAllocator(heap, true)).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"

namespace memory {

// What a RegionHeap reports (RegionHeap::stats()).
struct HeapStats {
    size_t total;         // bytes the blocks can use (the region less alignment and end marker)
    size_t used;          // bytes of the allocated blocks, headers included
    size_t peak;          // highest 'used' since init()
    size_t largestFree;   // largest request allocate() can serve now, bytes
    size_t usedBlocks;    // allocated blocks
    size_t freeBlocks;    // free blocks
};

// The header in front of each block (region_heap.cpp).
struct RegionBlock;

// A heap in a region given to init(). See above.
class RegionHeap {
public:
    /**
     * Takes the region [start, start + size) for the heap: everything free.
     * False if it is too small (a few dozen bytes) to hold one block. A
     * region above 2 GB is cut to 2 GB.
     */
    bool          init(void *start, size_t size);

    /**
     * A block of at least 'size' bytes, 8-byte aligned; nullptr if none is
     * free (or size is 0).
     */
    void         *allocate(size_t size);

    /**
     * Resizes a block, in place when it can, else by moving it (contents
     * kept); nullptr: a new block. Size 0 releases it and returns nullptr.
     * nullptr if no room: the block is then untouched.
     */
    void         *reallocate(void *block, size_t size);

    /**
     * Frees a block (nullptr: nothing). Fatal if the block is not one of this
     * heap's allocated blocks.
     */
    void          release(void *block);

    /**
     * Bytes of a block the caller may use (at least what was asked); 0 for
     * nullptr.
     */
    static size_t usableSize(const void *block);

    /**
     * Fills *out (walks the largest free list for largestFree).
     */
    void          stats(HeapStats *out) const;

    /**
     * Checks every block and every list (for the tests: walks the whole
     * heap). nullptr if consistent, else what is wrong (static text).
     */
    const char   *check() const;

private:
    static const int CLASS_COUNT = 128;               // size classes: 32 small, 96 large
    static const int BITMAP_WORDS = CLASS_COUNT / 32; // bits of the non-empty lists

    void          insert(RegionBlock *block);
    void          remove(RegionBlock *block);
    int           firstNonEmpty(int fromClass) const;
    size_t        splitAndUse(RegionBlock *block, size_t blockSize, size_t needed);
    void          shrink(RegionBlock *block, size_t blockSize, size_t needed);
    RegionBlock  *ownedBlock(void *payload) const;
    void          countUsed(size_t added, size_t removed);

    uintptr_t     _start = 0;                         // first block (aligned)
    RegionBlock  *_end = nullptr;                     // end marker: an allocated block of size 0
    RegionBlock  *_lists[CLASS_COUNT] = {};           // first free block of each class
    uint32_t      _bitmap[BITMAP_WORDS] = {};         // bit set: the list of that class is not empty
    size_t        _total = 0;                         // bytes of blocks (_end - _start)
    size_t        _used = 0;                          // bytes of allocated blocks
    size_t        _peak = 0;                          // highest _used
    size_t        _usedBlocks = 0;                    // allocated blocks
    size_t        _freeBlocks = 0;                    // blocks in the lists
};

/**
 * QuickJS's allocation functions over 'heap' (for JS_NewRuntime2, through
 * board::jsAllocator()), keeping QuickJS's counts and memory limit. One heap
 * for every runtime of the program; 'locked': each call takes a lock
 * (desktop, where runtimes may live on several threads). The heap must
 * outlive the runtimes.
 */
const JSMallocFunctions *jsAllocator(RegionHeap *heap, bool locked);

} // namespace memory
