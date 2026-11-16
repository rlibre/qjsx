/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// agent::Storage over two slots of a flash read in place (see
// slot_storage.h). Taken from the RP2350 port (board_rp2350.cpp), its flash
// calls behind slots::Flash.
#include "slot_storage.h"
#include "memory.h"

#include <stdio.h>
#include <string.h>

namespace slots {

/**
 * 1. the firmware must end before the package area;
 * 2. the page buffer of uploads, for good.
 */
bool SlotStorage::open(const Flash *flash, uintptr_t firmwareEnd) {
    if (_opened || _refused) {
        return _opened;
    }

    // the firmware must not reach the package area
    uintptr_t areaStart = (uintptr_t)flash->address(flash->areaOffset);
    if (firmwareEnd > areaStart) {
        fprintf(stderr, "qjsx: the firmware overlaps the package area (board_config.h)\n");
        _refused = true;
        return false;
    }

    // the page buffer (memory::allocate aligns to 8)
    _flash = flash;
    _page = (uint8_t *)memory::allocate(flash->pageSize);
    _slotSize = flash->areaSize / 2;
    _opened = true;
    return true;
}

/**
 * 1. the target: the slot that is not installed, if it holds the package
 *    and the program does not run from it;
 * 2. its header and the room of the package erased, sector by sector;
 * 3. the upload remembered.
 */
bool SlotStorage::beginUpload(size_t size) {
    _mutex.lock();

    // the target
    SlotHeader installedHeader;
    int installedSlot = findInstalled(&installedHeader);
    int target = installedSlot == 0 ? 1 : 0;

    bool fits = _opened && size > 0 && size <= _slotSize - headerSize();
    bool free = target != _mappedSlot;
    if (!fits || !free) {
        _mutex.unlock();
        return false;
    }

    // erase: the slot is now empty
    uint32_t sector = _flash->sectorSize;
    uint32_t erased = (headerSize() + (uint32_t)size + sector - 1) / sector * sector;
    bool ok = true;
    for (uint32_t done = 0; done < erased && ok; done += sector) {
        ok = _flash->eraseSector(slotOffset(target) + done);
    }

    // the upload: where, how big, which sequence it will take
    _uploadSlot = ok ? target : NO_SLOT;
    _uploadSize = size;
    _uploadNext = 0;
    _pageFill = 0;
    _uploadSequence = installedSlot == NO_SLOT ? 1 : installedHeader.sequence + 1;
    _mutex.unlock();
    return ok;
}

/**
 * Fills the page buffer, programs it each time it is full.
 */
bool SlotStorage::writeUpload(size_t offset, const uint8_t *data, size_t size) {
    bool inOrder = offset == _uploadNext;
    if (_uploadSlot == NO_SLOT || !inOrder || offset + size > _uploadSize) {
        return false;
    }

    while (size > 0) {
        // into the buffer
        size_t room = _flash->pageSize - _pageFill;
        size_t taken = size < room ? size : room;
        memcpy(_page + _pageFill, data, taken);
        _pageFill += taken;
        _uploadNext += taken;
        data += taken;
        size -= taken;

        // a full page: program it
        if (_pageFill == _flash->pageSize && !flushPage()) {
            return false;
        }
    }

    return true;
}

/**
 * 1. the last, partial page of the package;
 * 2. the header, written last as one page: until now the slot had no valid
 *    header.
 */
bool SlotStorage::finishUpload(const char *buildId) {
    if (_uploadSlot == NO_SLOT || _uploadNext != _uploadSize) {
        return false;
    }

    // the last page
    if (_pageFill > 0 && !flushPage()) {
        _uploadSlot = NO_SLOT;
        return false;
    }

    // the header, build id truncated to its field
    SlotHeader header = {};
    header.magic = PACKAGE_MAGIC;
    header.sequence = _uploadSequence;
    header.size = (uint32_t)_uploadSize;
    strncpy(header.buildId, buildId, sizeof(header.buildId) - 1);

    memset(_page, 0xff, _flash->pageSize);
    memcpy(_page, &header, sizeof(header));
    bool ok = _flash->programPage(slotOffset(_uploadSlot), _page);
    _uploadSlot = NO_SLOT;
    return ok;
}

/**
 * Forgets the upload slot.
 */
void SlotStorage::abortUpload() {
    _uploadSlot = NO_SLOT;
}

/**
 * Reads the installed slot's header under the lock.
 */
bool SlotStorage::installed(char *buildId, size_t buildIdSize, size_t *packageSize) {
    SlotHeader header;
    _mutex.lock();
    int slot = findInstalled(&header);
    _mutex.unlock();
    if (slot == NO_SLOT) {
        return false;
    }

    snprintf(buildId, buildIdSize, "%s", header.buildId);
    *packageSize = header.size;
    return true;
}

/**
 * 1. the installed slot, if any and if none is mapped yet;
 * 2. the package after its header.
 */
const uint8_t *SlotStorage::map(size_t *size) {
    _mutex.lock();

    // the installed slot
    SlotHeader header;
    int slot = findInstalled(&header);
    if (slot == NO_SLOT || _mappedSlot != NO_SLOT) {
        _mutex.unlock();
        return nullptr;
    }

    // the package
    _mappedSlot = slot;
    *size = header.size;
    const uint8_t *package = _flash->address(slotOffset(slot) + headerSize());
    _mutex.unlock();
    return package;
}

/**
 * Releases the mapped slot.
 */
void SlotStorage::unmap() {
    _mutex.lock();
    _mappedSlot = NO_SLOT;
    _mutex.unlock();
}

/**
 * The area's offset plus the slots before.
 */
uint32_t SlotStorage::slotOffset(int slot) const {
    return _flash->areaOffset + (uint32_t)slot * _slotSize;
}

/**
 * One sector, erased with the slot.
 */
uint32_t SlotStorage::headerSize() const {
    return _flash->sectorSize;
}

/**
 * The page the buffer ends: its data started _pageFill bytes before
 * _uploadNext.
 */
bool SlotStorage::flushPage() {
    size_t pageStart = _uploadNext - _pageFill;
    memset(_page + _pageFill, 0xff, _flash->pageSize - _pageFill);
    uint32_t offset = slotOffset(_uploadSlot) + headerSize() + (uint32_t)pageStart;

    _pageFill = 0;
    return _flash->programPage(offset, _page);
}

/**
 * A written header, a size that fits, a terminated build id.
 */
bool SlotStorage::readHeader(int slot, SlotHeader *header) {
    memcpy(header, _flash->address(slotOffset(slot)), sizeof(SlotHeader));

    bool written = header->magic == PACKAGE_MAGIC;
    bool fits = header->size > 0 && header->size <= _slotSize - headerSize();
    bool terminated = memchr(header->buildId, 0, sizeof(header->buildId)) != nullptr;
    return written && fits && terminated;
}

/**
 * Reads both headers; the valid one, or the later of two.
 */
int SlotStorage::findInstalled(SlotHeader *header) {
    if (!_opened) {
        return NO_SLOT;
    }

    // both headers
    SlotHeader first;
    SlotHeader second;
    bool firstValid = readHeader(0, &first);
    bool secondValid = readHeader(1, &second);

    // the valid one, or the later
    bool takeSecond = secondValid && (!firstValid || second.sequence > first.sequence);
    if (takeSecond) {
        *header = second;
        return 1;
    }

    if (firstValid) {
        *header = first;
        return 0;
    }

    return NO_SLOT;
}

} // namespace slots
