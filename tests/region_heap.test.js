/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tests of memory::RegionHeap (common/runtime/region_heap.*), the heap of a
// raw region (the JS heap in the PSRAM of the RP2350), through the native
// module "region_heap_test".
// Run with: bin/Release/qjsx-tests run tests/region_heap.test.js
import { TestHeap } from "region_heap_test";
import { check, equal, throws, report } from "./assert.js";

// --- a region too small is refused; a misaligned one is aligned
check(!new TestHeap(16, 0).ready(), "a 16-byte region is refused");
const misaligned = new TestHeap(4096, 3);
check(misaligned.ready(), "a misaligned region is accepted");
const first = misaligned.allocate(1);
check(misaligned.aligned(first), "blocks of a misaligned region are 8-byte aligned");
misaligned.check();

// --- one block: usable size, alignment, statistics
const heap = new TestHeap(65536, 0);
check(heap.ready(), "a 64 KB region is accepted");
const total = heap.total();
const emptyLargest = heap.largestFree();
check(total > 65000 && total <= 65536, "total is the region less the end marker");
equal(heap.usedBytes(), 0, "nothing used at first");
equal(heap.freeBlocks(), 1, "one free block at first");

const a = heap.allocate(100);
check(a >= 0, "allocate(100)");
check(heap.usable(a) >= 100 && heap.usable(a) < 100 + 16, "usable size is the request rounded up");
check(heap.aligned(a), "8-byte aligned");
equal(heap.usedBlocks(), 1, "one block used");
equal(heap.allocate(0), -1, "allocate(0) gives nothing");
heap.check();

// --- release merges with both neighbours: the heap is whole again
const b = heap.allocate(200);
const c = heap.allocate(300);
heap.release(b);
equal(heap.freeBlocks(), 2, "a hole and the rest");
heap.release(a);
heap.check();
heap.release(c);
heap.check();
equal(heap.freeBlocks(), 1, "everything merged into one free block");
equal(heap.usedBytes(), 0, "nothing used after releasing all");
equal(heap.largestFree(), emptyLargest, "the whole region is free again");
check(heap.peak() > 600, "the peak remembers the most used");

// --- exhaustion: no room gives -1, the heap stays usable
equal(heap.allocate(emptyLargest + 1), -1, "a request larger than the region");
const whole = heap.allocate(emptyLargest);
check(whole >= 0, "the whole region in one block");
equal(heap.allocate(1), -1, "nothing left");
heap.release(whole);
heap.check();

// --- reallocate: shrink and grow in place, move when needed
const r = heap.allocate(1000);
const neighbour = heap.allocate(64);
check(heap.reallocate(r, 500), "shrink");
heap.check();
check(heap.reallocate(r, 1000), "grow back in place (the freed tail)");
heap.check();
check(heap.reallocate(r, 5000), "grow by moving (the neighbour is in the way)");
heap.check();
equal(heap.reallocate(r, emptyLargest), false, "no room: the block stays");
heap.check();
heap.release(neighbour);
heap.release(r);
equal(heap.freeBlocks(), 1, "whole again after the resizes");

// --- misuse is caught (by the test module, before the heap sees it)
throws(() => heap.release(0), /slot 0 is not an intact block/, "releasing a free slot");

// --- random stress: small and large blocks, checked every 64 operations
for (const seed of [1, 2, 3, 42, 1234]) {
    const stressed = new TestHeap(256 * 1024, seed % 8);
    const refused = stressed.stress(seed, 20000, 8192);
    stressed.check();
    stressed.releaseAll();
    stressed.check();
    equal(stressed.freeBlocks(), 1, `seed ${seed}: one free block after releasing all`);
    check(refused >= 0, `seed ${seed}: ${refused} allocations found no room`);
}

// --- a nearly full heap: many refusals, still consistent
const full = new TestHeap(32 * 1024, 0);
const refusedFull = full.stress(7, 20000, 4096);
check(refusedFull > 0, "a small heap refuses some allocations");
full.check();

report("region heap");
