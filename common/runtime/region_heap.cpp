/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A heap in a raw region of memory (see region_heap.h).
//
// Layout of the region:
//
//   [block][block]...[block][end marker]
//
// Each block starts with a RegionBlock header. The end marker is a header of
// size 0 flagged as allocated: the last block has a "next" like the others,
// and it is never merged. A free block holds, in its payload, its links in the
// list of its class, and in its last word its size (read by the block after
// it to find it when merging).
#include "region_heap.h"
#include "platform.h"

#include <string.h>

namespace memory {

// The header in front of each block (8 bytes: the payload stays 8-byte
// aligned).
struct RegionBlock {
    uint32_t sizeAndFlags;   // bytes of the block, header included (multiple of 8) | flags
    uint32_t check;          // the block's address mixed with USED_CHECK or FREE_CHECK
};

namespace {

// Links of a free block in the list of its class, in its payload.
struct FreeLinks {
    RegionBlock *next;       // next free block of the class, nullptr at the end
    RegionBlock *previous;   // previous one, nullptr for the first
};

const size_t   ALIGNMENT = 8;                 // of every block and payload
const uint32_t USED = 1;                      // flag: the block is allocated
const uint32_t PREVIOUS_FREE = 2;             // flag: the block before it is free
const uint32_t FLAGS = ALIGNMENT - 1;         // low bits of sizeAndFlags
const uint32_t USED_CHECK = 0x51a7b10c;       // check word of an allocated block (with its address)
const uint32_t FREE_CHECK = 0xf4eeb10c;       // check word of a free block
const size_t   LARGEST_REGION = 0x80000000;   // classes go up to 2 GB

// Size classes: 32 small classes of 8 bytes (sizes 0 to 248), then from 256
// each power of two split in four.
const size_t SMALL_LIMIT = 256;               // first size of the large classes
const int    SMALL_CLASSES = 32;              // SMALL_LIMIT / ALIGNMENT
const int    SMALL_LIMIT_LOG2 = 8;            // log2(SMALL_LIMIT)
const int    SUBDIVISION_BITS = 2;            // each power of two split in 4

/**
 * Bytes rounded up to the alignment.
 */
size_t alignUp(size_t size) {
    return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

// Smallest block: header, links when free, size in the last word.
const size_t MINIMUM_BLOCK = (sizeof(RegionBlock) + sizeof(FreeLinks) + sizeof(uint32_t) + ALIGNMENT - 1) & ~(ALIGNMENT - 1);

/**
 * Size of a block (flags removed).
 */
size_t sizeOf(const RegionBlock *block) {
    return block->sizeAndFlags & ~FLAGS;
}

/**
 * The block after 'block' in the region.
 */
RegionBlock *nextOf(const RegionBlock *block) {
    return (RegionBlock *)((uintptr_t)block + sizeOf(block));
}

/**
 * The links of a free block (its payload).
 */
FreeLinks *linksOf(RegionBlock *block) {
    return (FreeLinks *)(block + 1);
}

/**
 * The last word of a free block: its size.
 */
uint32_t *footerOf(RegionBlock *block, size_t size) {
    return (uint32_t *)((uintptr_t)block + size - sizeof(uint32_t));
}

/**
 * The check word of a block at this address, allocated or free.
 */
uint32_t checkWord(const RegionBlock *block, uint32_t kind) {
    return (uint32_t)(uintptr_t)block ^ kind;
}

/**
 * The size class of a block size: exact below SMALL_LIMIT, then by power of
 * two and quarter.
 */
int classOf(size_t size) {
    if (size < SMALL_LIMIT) {
        return (int)(size / ALIGNMENT);
    }

    // the power of two, then which quarter of it
    int power = 31 - __builtin_clz((uint32_t)size);
    int quarter = (int)(size >> (power - SUBDIVISION_BITS)) & ((1 << SUBDIVISION_BITS) - 1);
    return SMALL_CLASSES + ((power - SMALL_LIMIT_LOG2) << SUBDIVISION_BITS) + quarter;
}

/**
 * Smallest block size of a class.
 */
size_t classStart(int sizeClass) {
    if (sizeClass < SMALL_CLASSES) {
        return (size_t)sizeClass * ALIGNMENT;
    }

    int large = sizeClass - SMALL_CLASSES;
    int power = SMALL_LIMIT_LOG2 + (large >> SUBDIVISION_BITS);
    int quarter = large & ((1 << SUBDIVISION_BITS) - 1);
    return ((size_t)1 << power) + (size_t)quarter * ((size_t)1 << (power - SUBDIVISION_BITS));
}

/**
 * First class whose every block holds 'size' bytes: the class of 'size' if
 * it starts exactly there, else the next one.
 */
int searchClass(size_t size) {
    int sizeClass = classOf(size);
    if (classStart(sizeClass) < size) {
        sizeClass++;
    }

    return sizeClass;
}

/**
 * Size of the block that serves a request of 'size' bytes (header added,
 * aligned, at least MINIMUM_BLOCK); 0 if too large.
 */
size_t blockSizeFor(size_t size) {
    if (size == 0 || size > LARGEST_REGION / 2) {
        return 0;
    }

    size_t needed = alignUp(size + sizeof(RegionBlock));
    return needed < MINIMUM_BLOCK ? MINIMUM_BLOCK : needed;
}

/**
 * Marks a block of 'size' bytes free: header, footer, and the flag of the
 * block after it. Free blocks never follow a free block: no PREVIOUS_FREE.
 */
void markFree(RegionBlock *block, size_t size) {
    block->sizeAndFlags = (uint32_t)size;
    block->check = checkWord(block, FREE_CHECK);
    *footerOf(block, size) = (uint32_t)size;
    nextOf(block)->sizeAndFlags |= PREVIOUS_FREE;
}

/**
 * Marks a block of 'size' bytes allocated, keeping its PREVIOUS_FREE flag
 * ('previousFree'), and clears the flag of the block after it.
 */
void markUsed(RegionBlock *block, size_t size, uint32_t previousFree) {
    block->sizeAndFlags = (uint32_t)size | USED | previousFree;
    block->check = checkWord(block, USED_CHECK);
    nextOf(block)->sizeAndFlags &= ~PREVIOUS_FREE;
}

} // namespace

// ### RegionHeap ##############################################################

/**
 * 1. align the region, keep room for the end marker;
 * 2. the end marker: an allocated block of size 0;
 * 3. one free block for all the rest.
 */
bool RegionHeap::init(void *start, size_t size) {
    // the region, aligned at both ends, at most LARGEST_REGION
    uintptr_t first = alignUp((uintptr_t)start);
    uintptr_t limit = ((uintptr_t)start + size) & ~(uintptr_t)FLAGS;
    if (limit <= first || limit - first < MINIMUM_BLOCK + sizeof(RegionBlock)) {
        return false;
    }

    if (limit - first > LARGEST_REGION) {
        limit = first + LARGEST_REGION;
    }

    // empty lists
    memset(_lists, 0, sizeof(_lists));
    memset(_bitmap, 0, sizeof(_bitmap));
    _used = 0;
    _peak = 0;
    _usedBlocks = 0;
    _freeBlocks = 0;

    // the end marker
    _start = first;
    _end = (RegionBlock *)(limit - sizeof(RegionBlock));
    _end->sizeAndFlags = USED;
    _end->check = checkWord(_end, USED_CHECK);

    // one free block before it
    RegionBlock *block = (RegionBlock *)first;
    _total = (uintptr_t)_end - first;
    markFree(block, _total);
    insert(block);
    return true;
}

/**
 * 1. the block size needed, the first non-empty class whose every block
 *    serves it, and its first block;
 * 2. none (the heap is nearly full): a block big enough in the class of the
 *    size itself, whose blocks may be smaller (a walk of that one list);
 * 3. take the block out of its list and split it: the part not needed goes
 *    back to the lists.
 */
void *RegionHeap::allocate(size_t size) {
    // the class to take from
    size_t needed = blockSizeFor(size);
    if (needed == 0) {
        return nullptr;
    }

    RegionBlock *block = nullptr;
    int sizeClass = firstNonEmpty(searchClass(needed));
    if (sizeClass >= 0) {
        block = _lists[sizeClass];
    }

    // else the class of the size itself, walked
    if (block == nullptr) {
        RegionBlock *candidate = _lists[classOf(needed)];
        while (candidate != nullptr && sizeOf(candidate) < needed) {
            candidate = linksOf(candidate)->next;
        }

        block = candidate;
    }

    if (block == nullptr) {
        return nullptr;
    }

    // out of its list
    remove(block);

    // used, the rest freed
    size_t used = splitAndUse(block, sizeOf(block), needed);
    countUsed(used, 0);
    _usedBlocks++;
    return block + 1;
}

/**
 * 1. nullptr or size 0: allocate or release;
 * 2. big enough: shrink in place;
 * 3. the next block is free and makes it big enough: grow in place;
 * 4. else move: a new block, copy, release the old one.
 */
void *RegionHeap::reallocate(void *payload, size_t size) {
    // the cases of allocate / release
    if (payload == nullptr) {
        return allocate(size);
    }

    if (size == 0) {
        release(payload);
        return nullptr;
    }

    RegionBlock *block = ownedBlock(payload);
    size_t needed = blockSizeFor(size);
    if (needed == 0) {
        return nullptr;
    }

    // shrink in place
    size_t blockSize = sizeOf(block);
    if (blockSize >= needed) {
        shrink(block, blockSize, needed);
        return payload;
    }

    // grow in place, over the next block if free and big enough
    RegionBlock *next = nextOf(block);
    bool nextFree = (next->sizeAndFlags & USED) == 0;
    if (nextFree && blockSize + sizeOf(next) >= needed) {
        size_t combined = blockSize + sizeOf(next);
        remove(next);
        size_t used = splitAndUse(block, combined, needed);
        countUsed(used, blockSize);
        return payload;
    }

    // move: the old block stays if there is no room
    void *moved = allocate(size);
    if (moved == nullptr) {
        return nullptr;
    }

    memcpy(moved, payload, blockSize - sizeof(RegionBlock));
    release(payload);
    return moved;
}

/**
 * 1. check that the heap owns the block, allocated;
 * 2. merge it with the next block and the previous one when free;
 * 3. the merged block goes into the lists.
 */
void RegionHeap::release(void *payload) {
    if (payload == nullptr) {
        return;
    }

    // ours and allocated (else fatal)
    RegionBlock *block = ownedBlock(payload);
    size_t size = sizeOf(block);
    countUsed(0, size);
    _usedBlocks--;

    // merge with the next block if free (the end marker is never free)
    RegionBlock *next = nextOf(block);
    if ((next->sizeAndFlags & USED) == 0) {
        remove(next);
        size += sizeOf(next);
    }

    // merge with the previous block if free: its size is in its last word,
    // just before this block
    if ((block->sizeAndFlags & PREVIOUS_FREE) != 0) {
        uint32_t previousSize = *(uint32_t *)((uintptr_t)block - sizeof(uint32_t));
        RegionBlock *previous = (RegionBlock *)((uintptr_t)block - previousSize);
        remove(previous);
        size += previousSize;
        block = previous;
    }

    // free, in the lists
    markFree(block, size);
    insert(block);
}

/**
 * The block size less the header.
 */
size_t RegionHeap::usableSize(const void *payload) {
    if (payload == nullptr) {
        return 0;
    }

    const RegionBlock *block = (const RegionBlock *)payload - 1;
    return sizeOf(block) - sizeof(RegionBlock);
}

/**
 * Counters, and the largest block of the highest non-empty class.
 */
void RegionHeap::stats(HeapStats *out) const {
    // the highest non-empty class
    int highest = -1;
    for (int word = BITMAP_WORDS - 1; word >= 0 && highest < 0; word--) {
        if (_bitmap[word] != 0) {
            highest = word * 32 + 31 - __builtin_clz(_bitmap[word]);
        }
    }

    // its largest block (a large class holds sizes of a whole quarter)
    size_t largest = 0;
    if (highest >= 0) {
        for (RegionBlock *block = _lists[highest]; block != nullptr; block = linksOf(block)->next) {
            size_t size = sizeOf(block);
            largest = size > largest ? size : largest;
        }
    }

    *out = HeapStats{ _total, _used, _peak, largest > 0 ? largest - sizeof(RegionBlock) : 0, _usedBlocks, _freeBlocks };
}

/**
 * 1. walk the blocks: sizes, flags, check words, footers, no two free blocks
 *    in a row, the used bytes;
 * 2. walk the lists: each block free, in its class, linked both ways, the
 *    bitmap bit set exactly for the non-empty lists;
 * 3. both walks agree with the counters.
 */
const char *RegionHeap::check() const {
    // the blocks, in order
    size_t usedBytes = 0;
    size_t usedBlocks = 0;
    size_t freeBlocks = 0;
    bool previousFree = false;
    RegionBlock *block = (RegionBlock *)_start;

    while (block != _end) {
        size_t size = sizeOf(block);
        bool used = (block->sizeAndFlags & USED) != 0;
        bool flaggedPreviousFree = (block->sizeAndFlags & PREVIOUS_FREE) != 0;
        if (size < MINIMUM_BLOCK || (uintptr_t)block + size > (uintptr_t)_end) {
            return "a block has a wrong size";
        }

        if (flaggedPreviousFree != previousFree) {
            return "a PREVIOUS_FREE flag is wrong";
        }

        if (used) {
            if (block->check != checkWord(block, USED_CHECK)) {
                return "an allocated block has a wrong check word";
            }

            usedBytes += size;
            usedBlocks++;
        } else {
            if (block->check != checkWord(block, FREE_CHECK)) {
                return "a free block has a wrong check word";
            }

            if (*footerOf(block, size) != size) {
                return "a free block has a wrong footer";
            }

            if (previousFree) {
                return "two free blocks in a row";
            }

            freeBlocks++;
        }

        previousFree = !used;
        block = nextOf(block);
    }

    bool endFlagged = (_end->sizeAndFlags & PREVIOUS_FREE) != 0;
    if (endFlagged != previousFree) {
        return "the PREVIOUS_FREE flag of the end marker is wrong";
    }

    // the lists
    size_t listed = 0;
    for (int sizeClass = 0; sizeClass < CLASS_COUNT; sizeClass++) {
        bool bit = (_bitmap[sizeClass / 32] & (1u << (sizeClass % 32))) != 0;
        if (bit != (_lists[sizeClass] != nullptr)) {
            return "a bitmap bit does not match its list";
        }

        RegionBlock *previous = nullptr;
        for (RegionBlock *entry = _lists[sizeClass]; entry != nullptr; entry = linksOf(entry)->next) {
            if ((entry->sizeAndFlags & USED) != 0 || classOf(sizeOf(entry)) != sizeClass) {
                return "a listed block is allocated or in the wrong class";
            }

            if (linksOf(entry)->previous != previous) {
                return "a list has a wrong back link";
            }

            previous = entry;
            listed++;
        }
    }

    // the counters
    if (listed != freeBlocks || listed != _freeBlocks) {
        return "free blocks and lists do not agree";
    }

    if (usedBytes != _used || usedBlocks != _usedBlocks) {
        return "the used counters are wrong";
    }

    return nullptr;
}

/**
 * Puts a free block first in the list of its class.
 */
void RegionHeap::insert(RegionBlock *block) {
    int sizeClass = classOf(sizeOf(block));
    RegionBlock *first = _lists[sizeClass];

    linksOf(block)->next = first;
    linksOf(block)->previous = nullptr;
    if (first != nullptr) {
        linksOf(first)->previous = block;
    }

    _lists[sizeClass] = block;
    _bitmap[sizeClass / 32] |= 1u << (sizeClass % 32);
    _freeBlocks++;
}

/**
 * Takes a free block out of the list of its class.
 */
void RegionHeap::remove(RegionBlock *block) {
    int sizeClass = classOf(sizeOf(block));
    FreeLinks *links = linksOf(block);

    // unlink it
    if (links->previous != nullptr) {
        linksOf(links->previous)->next = links->next;
    } else {
        _lists[sizeClass] = links->next;
    }

    if (links->next != nullptr) {
        linksOf(links->next)->previous = links->previous;
    }

    // an empty list: its bit goes
    if (_lists[sizeClass] == nullptr) {
        _bitmap[sizeClass / 32] &= ~(1u << (sizeClass % 32));
    }

    _freeBlocks--;
}

/**
 * The first non-empty class from 'fromClass' on (bitmap words, lowest bit
 * first); -1 if none.
 */
int RegionHeap::firstNonEmpty(int fromClass) const {
    if (fromClass >= CLASS_COUNT) {
        return -1;
    }

    for (int word = fromClass / 32; word < BITMAP_WORDS; word++) {
        // in the first word, only the classes from fromClass
        uint32_t bits = _bitmap[word];
        if (word == fromClass / 32) {
            bits &= ~0u << (fromClass % 32);
        }

        if (bits != 0) {
            return word * 32 + __builtin_ctz(bits);
        }
    }

    return -1;
}

/**
 * Makes 'block' (blockSize bytes, out of the lists, followed by an allocated
 * block) an allocated block of 'needed' bytes; what is left goes back to the
 * lists if it makes a block. Returns the final size of the block.
 */
size_t RegionHeap::splitAndUse(RegionBlock *block, size_t blockSize, size_t needed) {
    uint32_t previousFree = block->sizeAndFlags & PREVIOUS_FREE;

    // too little left for a block: all of it is used
    if (blockSize - needed < MINIMUM_BLOCK) {
        markUsed(block, blockSize, previousFree);
        return blockSize;
    }

    // the needed part used, the rest free (followed by an allocated block)
    markUsed(block, needed, previousFree);
    RegionBlock *rest = nextOf(block);
    markFree(rest, blockSize - needed);
    insert(rest);
    return needed;
}

/**
 * Shrinks an allocated block to 'needed' bytes if the rest makes a block;
 * the rest is freed, merged with the next block if free.
 */
void RegionHeap::shrink(RegionBlock *block, size_t blockSize, size_t needed) {
    if (blockSize - needed < MINIMUM_BLOCK) {
        return;
    }

    // the rest, and the free block after it if any
    RegionBlock *rest = (RegionBlock *)((uintptr_t)block + needed);
    size_t restSize = blockSize - needed;
    RegionBlock *next = (RegionBlock *)((uintptr_t)rest + restSize);
    if ((next->sizeAndFlags & USED) == 0) {
        remove(next);
        restSize += sizeOf(next);
    }

    // the block smaller, the rest free (markUsed clears the flag of the
    // rest's header, which markFree then writes whole)
    markUsed(block, needed, block->sizeAndFlags & PREVIOUS_FREE);
    markFree(rest, restSize);
    insert(rest);
    countUsed(0, blockSize - needed);
}

/**
 * The header of a payload, after checking that it is an allocated block of
 * this heap: fatal otherwise (a wrong pointer, or a block released twice).
 */
RegionBlock *RegionHeap::ownedBlock(void *payload) const {
    RegionBlock *block = (RegionBlock *)payload - 1;
    bool inside = (uintptr_t)block >= _start && block < _end;
    if (!inside) {
        platform::fatal("region heap: a block it does not own");
    }

    if (block->check == checkWord(block, FREE_CHECK)) {
        platform::fatal("region heap: a block released twice");
    }

    if (block->check != checkWord(block, USED_CHECK)) {
        platform::fatal("region heap: a damaged block");
    }

    return block;
}

/**
 * Updates the used bytes and their peak.
 */
void RegionHeap::countUsed(size_t added, size_t removed) {
    _used = _used + added - removed;
    _peak = _used > _peak ? _used : _peak;
}

// ### QuickJS allocator #######################################################

namespace {

RegionHeap     *jsHeap = nullptr;   // the heap of jsAllocator()
bool            jsLocked = false;   // calls take jsLock
platform::Mutex jsLock;             // when jsLocked

/**
 * Takes jsLock when the calls must be locked.
 */
void lockJsHeap() {
    if (jsLocked) {
        jsLock.lock();
    }
}

/**
 * Releases what lockJsHeap() took.
 */
void unlockJsHeap() {
    if (jsLocked) {
        jsLock.unlock();
    }
}

/**
 * Bytes QuickJS counts for a block: its usable size and its header, as its
 * own allocator counts malloc's overhead.
 */
size_t countedSize(const void *payload) {
    return RegionHeap::usableSize(payload) + sizeof(RegionBlock);
}

/**
 * QuickJS allocation: refused past QuickJS's memory limit.
 */
void *jsMalloc(JSMallocState *state, size_t size) {
    if (state->malloc_size + size > state->malloc_limit) {
        return nullptr;
    }

    lockJsHeap();
    void *payload = jsHeap->allocate(size);
    unlockJsHeap();
    if (payload == nullptr) {
        return nullptr;
    }

    state->malloc_count++;
    state->malloc_size += countedSize(payload);
    return payload;
}

/**
 * QuickJS free (nullptr: nothing).
 */
void jsFree(JSMallocState *state, void *payload) {
    if (payload == nullptr) {
        return;
    }

    state->malloc_count--;
    state->malloc_size -= countedSize(payload);

    lockJsHeap();
    jsHeap->release(payload);
    unlockJsHeap();
}

/**
 * QuickJS resize (nullptr: allocate; size 0: free), refused past QuickJS's
 * memory limit.
 */
void *jsRealloc(JSMallocState *state, void *payload, size_t size) {
    // the cases of malloc and free
    if (payload == nullptr) {
        return size == 0 ? nullptr : jsMalloc(state, size);
    }

    if (size == 0) {
        jsFree(state, payload);
        return nullptr;
    }

    // within the limit (as QuickJS's own allocator: malloc_size counts
    // oldSize, so the subtraction cannot wrap)
    size_t oldSize = countedSize(payload);
    if (state->malloc_size + size - oldSize > state->malloc_limit) {
        return nullptr;
    }

    // resize, then count the difference
    lockJsHeap();
    void *resized = jsHeap->reallocate(payload, size);
    unlockJsHeap();
    if (resized == nullptr) {
        return nullptr;
    }

    state->malloc_size = state->malloc_size - oldSize + countedSize(resized);
    return resized;
}

/**
 * QuickJS's usable size of a block.
 */
size_t jsUsableSize(const void *payload) {
    return RegionHeap::usableSize(payload);
}

const JSMallocFunctions regionFunctions = { &jsMalloc, &jsFree, &jsRealloc, &jsUsableSize };

} // namespace

/**
 * Remembers the heap and the locking, returns the functions using them.
 */
const JSMallocFunctions *jsAllocator(RegionHeap *heap, bool locked) {
    jsHeap = heap;
    jsLocked = locked;
    return &regionFunctions;
}

} // namespace memory
