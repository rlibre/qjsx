/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Native module "region_heap_test": drives a memory::RegionHeap from JS
// (tests/region_heap.test.js). Only built into qjsx-tests.
//
// A TestHeap owns a region and a table of slots. Each allocated block is
// filled with a byte pattern of its slot, checked again before it is
// released or resized, so a block that overlaps another one is seen.
#include <string.h>

#include "jsbind.h"
#include "memory.h"
#include "modules.h"
#include "region_heap.h"

namespace {

const int SLOT_COUNT = 4096;   // blocks a TestHeap can hold at once

// One block of the table.
struct Slot {
    uint8_t *payload;   // nullptr: free slot
    size_t   size;      // bytes asked (filled with the pattern)
};

/**
 * The byte pattern of a slot.
 */
uint8_t patternOf(int slot) {
    return (uint8_t)(slot * 31 + 7);
}

/**
 * A RegionHeap in its own region, with a table of blocks (JS class TestHeap).
 */
class TestHeap {
    JS_CLASS(TestHeap);

public:
    /**
     * Frees the region (no JS here).
     */
    ~TestHeap() {
        memory::release(_region);
        memory::release(_slots);
    }

    /**
     * JS constructor: new TestHeap(size, offset): a heap over 'size' bytes
     * starting 'offset' bytes after an aligned address (to test alignment).
     */
    void init(js::Range<int, 16, 67108864> size, js::Range<int, 0, 7> offset) {
        _region = (uint8_t *)memory::allocate((size_t)(int)size + 8);
        _slots = (Slot *)memory::allocate(sizeof(Slot) * SLOT_COUNT);
        memset(_slots, 0, sizeof(Slot) * SLOT_COUNT);
        _ready = _heap.init(_region + (int)offset, (size_t)(int)size);
    }

    /**
     * Whether init() accepted the region.
     */
    JS_EXPORT(ready) bool ready() {
        return _ready;
    }

    /**
     * Allocates 'size' bytes into a free slot, filled with its pattern.
     * The slot number, or -1 if the heap has no room.
     */
    JS_EXPORT(allocate) int allocate(js::Range<int, 0, 67108864> size) {
        int slot = freeSlot();
        if (slot < 0) {
            return js::fail("TestHeap: no free slot");
        }

        uint8_t *payload = (uint8_t *)_heap.allocate((size_t)(int)size);
        if (payload == nullptr) {
            return -1;
        }

        _slots[slot] = Slot{ payload, (size_t)(int)size };
        memset(payload, patternOf(slot), _slots[slot].size);
        return slot;
    }

    /**
     * Resizes the block of a slot (pattern checked before, written again
     * after). False if the heap has no room (the block must be intact).
     */
    JS_EXPORT(reallocate) bool reallocate(int slot, js::Range<int, 1, 67108864> size) {
        if (!occupied(slot) || !intact(slot)) {
            return js::fail("TestHeap: slot %d is not an intact block", slot);
        }

        uint8_t *payload = (uint8_t *)_heap.reallocate(_slots[slot].payload, (size_t)(int)size);
        if (payload == nullptr) {
            return false;
        }

        // the kept part must have moved with the block
        size_t kept = _slots[slot].size < (size_t)(int)size ? _slots[slot].size : (size_t)(int)size;
        _slots[slot] = Slot{ payload, kept };
        if (!intact(slot)) {
            return js::fail("TestHeap: slot %d lost its contents in reallocate", slot);
        }

        _slots[slot].size = (size_t)(int)size;
        memset(payload, patternOf(slot), _slots[slot].size);
        return true;
    }

    /**
     * Releases the block of a slot (pattern checked first).
     */
    JS_EXPORT(release) void release(int slot) {
        if (!occupied(slot) || !intact(slot)) {
            return (void)js::fail("TestHeap: slot %d is not an intact block", slot);
        }

        _heap.release(_slots[slot].payload);
        _slots[slot] = Slot{ nullptr, 0 };
    }

    /**
     * Releases every block.
     */
    JS_EXPORT(releaseAll) void releaseAll() {
        for (int slot = 0; slot < SLOT_COUNT; slot++) {
            if (occupied(slot)) {
                release(slot);
            }
        }
    }

    /**
     * usableSize() of a slot's block.
     */
    JS_EXPORT(usable) int usable(int slot) {
        if (!occupied(slot)) {
            return js::fail("TestHeap: slot %d is free", slot);
        }

        return (int)memory::RegionHeap::usableSize(_slots[slot].payload);
    }

    /**
     * Whether a slot's block is 8-byte aligned.
     */
    JS_EXPORT(aligned) bool aligned(int slot) {
        return occupied(slot) && ((uintptr_t)_slots[slot].payload & 7) == 0;
    }

    /**
     * The heap's own check() and every block's pattern; throws what is
     * wrong.
     */
    JS_EXPORT(check) bool check() {
        const char *problem = _heap.check();
        if (problem != nullptr) {
            return js::fail("TestHeap: %s", problem);
        }

        for (int slot = 0; slot < SLOT_COUNT; slot++) {
            if (occupied(slot) && !intact(slot)) {
                return js::fail("TestHeap: slot %d was overwritten", slot);
            }
        }

        return true;
    }

    /**
     * 'operations' random allocations, releases and resizes (seeded),
     * checking the whole heap every 64 operations and at the end. Returns
     * how many allocations found no room.
     */
    JS_EXPORT(stress) int stress(int seed, js::Range<int, 1, 10000000> operations, js::Range<int, 1, 67108864> largest) {
        uint32_t random = (uint32_t)seed * 2654435761u + 1;
        int refused = 0;

        for (int done = 0; done < (int)operations; done++) {
            // the next random number (xorshift)
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;

            // a size: mostly small, sometimes up to 'largest'
            bool big = random % 16 == 0;
            int size = big ? 1 + (int)((random >> 8) % (uint32_t)(int)largest) : 1 + (int)((random >> 8) % 300);
            int slot = (int)((random >> 4) % SLOT_COUNT);

            // allocate into a free slot, else release or resize what is there
            if (!occupied(slot)) {
                uint8_t *payload = (uint8_t *)_heap.allocate((size_t)size);
                if (payload == nullptr) {
                    refused++;
                } else {
                    _slots[slot] = Slot{ payload, (size_t)size };
                    memset(payload, patternOf(slot), (size_t)size);
                }
            } else if (random % 3 == 0) {
                if (!reallocate(slot, size) && js::failed()) {
                    return 0;
                }
            } else {
                release(slot);
                if (js::failed()) {
                    return 0;
                }
            }

            // the whole heap, now and then
            bool due = done % 64 == 63 || done == (int)operations - 1;
            if (due && !check()) {
                return 0;
            }
        }

        return refused;
    }

    /**
     * HeapStats.total.
     */
    JS_EXPORT(total) int total() {
        return (int)stats().total;
    }

    /**
     * HeapStats.used.
     */
    JS_EXPORT(usedBytes) int usedBytes() {
        return (int)stats().used;
    }

    /**
     * HeapStats.peak.
     */
    JS_EXPORT(peak) int peak() {
        return (int)stats().peak;
    }

    /**
     * HeapStats.largestFree.
     */
    JS_EXPORT(largestFree) int largestFree() {
        return (int)stats().largestFree;
    }

    /**
     * HeapStats.usedBlocks.
     */
    JS_EXPORT(usedBlocks) int usedBlocks() {
        return (int)stats().usedBlocks;
    }

    /**
     * HeapStats.freeBlocks.
     */
    JS_EXPORT(freeBlocks) int freeBlocks() {
        return (int)stats().freeBlocks;
    }

private:
    /**
     * The heap's statistics.
     */
    memory::HeapStats stats() {
        memory::HeapStats result;
        _heap.stats(&result);
        return result;
    }

    /**
     * Whether a slot holds a block.
     */
    bool occupied(int slot) {
        return slot >= 0 && slot < SLOT_COUNT && _slots[slot].payload != nullptr;
    }

    /**
     * Whether a slot's block still holds its pattern.
     */
    bool intact(int slot) {
        uint8_t pattern = patternOf(slot);
        for (size_t i = 0; i < _slots[slot].size; i++) {
            if (_slots[slot].payload[i] != pattern) {
                return false;
            }
        }

        return true;
    }

    /**
     * The first free slot, or -1.
     */
    int freeSlot() {
        for (int slot = 0; slot < SLOT_COUNT; slot++) {
            if (!occupied(slot)) {
                return slot;
            }
        }

        return -1;
    }

    memory::RegionHeap _heap;              // under test
    uint8_t           *_region = nullptr;  // owned: the region (+ 8 bytes for the offset)
    Slot              *_slots = nullptr;   // owned: SLOT_COUNT slots
    bool               _ready = false;     // init() of the heap succeeded
};

} // namespace

/**
 * Module "region_heap_test": the class TestHeap.
 */
JS_MODULE(region_heap_test, module) {
    JS_ADD_CLASS(module, TestHeap);
}

/**
 * Registers the module "region_heap_test"; called by registerTestModules().
 */
void registerRegionHeapTestModule() {
    rt::registerNativeModule("region_heap_test", &jsNewModule_region_heap_test);
}
