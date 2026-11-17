/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// agent::Storage over two slots of a flash read in place (XIP), for the
// ports whose flash is written sector by sector and page by page (i.MX
// RT1062; the RP2350 has its own copy until its pass: board_rp2350.cpp).
//
// Each slot starts with a header sector, followed by the package. An upload
// goes into the slot that is not installed, and its header is written last:
// an interrupted upload leaves the installed package as it was, and the
// running program (read in place from its slot) is never erased under it.
// Same layout as the ESP32 port.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent.h"
#include "platform.h"

namespace slots {

// What the storage needs from the port's flash.
struct Flash {
    uint32_t areaOffset;                                         // start of the two slots, from the start of the flash
    uint32_t areaSize;                                           // bytes of both slots
    uint32_t sectorSize;                                         // erase unit
    uint32_t pageSize;                                           // program unit (a slot header fits in one page)
    bool (*eraseSector)(uint32_t offset);                        // erases the sector at 'offset'
    bool (*programPage)(uint32_t offset, const uint8_t *page);   // programs pageSize bytes, 'page' in RAM, 4-byte aligned
    const uint8_t *(*address)(uint32_t offset);                  // the offset in the XIP window
};

// agent::Storage on the package area. Called on the link task (uploads) and
// on the JS task (map, installed): _mutex protects the slot numbers. The
// flash is programmed by pages: the upload goes through a page buffer, in
// order.
class SlotStorage : public agent::Storage {
public:
    /**
     * Uses 'flash' (static, borrowed for good); 'firmwareEnd' is the address
     * of the end of the firmware in the XIP window: it must end before the
     * package area. False otherwise, after saying so. Once: later calls
     * return the first answer.
     */
    bool                  open(const Flash *flash, uintptr_t firmwareEnd);

    /**
     * Erases the slot that is neither installed nor running, for a package
     * of 'size' bytes. False if it does not fit, or if both slots are busy.
     */
    bool                  beginUpload(size_t size) override;

    /**
     * Writes a chunk after the header of the upload slot; the chunks come in
     * order.
     */
    bool                  writeUpload(size_t offset, const uint8_t *data, size_t size) override;

    /**
     * Programs what is left of the package, then the header of the upload
     * slot: from now on it is the installed package.
     */
    bool                  finishUpload(const char *buildId) override;

    /**
     * Drops the upload: its slot has no header, the installed one stays.
     */
    void                  abortUpload() override;

    /**
     * Build id and size of the installed package. False if none.
     */
    bool                  installed(char *buildId, size_t buildIdSize, size_t *packageSize) override;

    /**
     * The installed package in the XIP window (read in place), nullptr if
     * none; one at a time (its slot is then never erased).
     */
    const uint8_t        *map(size_t *size);

    /**
     * Forgets what map() returned: its slot may be erased again.
     */
    void                  unmap();

private:
    // Header of a slot, at its start.
    struct SlotHeader {
        uint32_t magic;                   // PACKAGE_MAGIC when the slot holds a package
        uint32_t sequence;                // higher: installed later
        uint32_t size;                    // bytes of the package after the header
        char     buildId[64];             // zero-terminated
    };

    /**
     * Offset of a slot from the start of the flash.
     */
    uint32_t              slotOffset(int slot) const;

    /**
     * Bytes of the header area of a slot: one sector.
     */
    uint32_t              headerSize() const;

    /**
     * Programs the page buffer at its place in the upload slot (padded with
     * 0xff), then empties it.
     */
    bool                  flushPage();

    /**
     * Reads the header of a slot; true if it holds a package.
     */
    bool                  readHeader(int slot, SlotHeader *header);

    /**
     * The installed slot (the valid one with the highest sequence) and its
     * header, or NO_SLOT. _mutex held.
     */
    int                   findInstalled(SlotHeader *header);

    static const uint32_t PACKAGE_MAGIC = 0x4b504a45;   // "EJPK" in little endian
    static const int      NO_SLOT = -1;                 // none (installed, uploaded or mapped)

    const Flash          *_flash = nullptr;          // the port's flash; borrowed
    bool                  _opened = false;           // open() succeeded
    bool                  _refused = false;          // open() refused (firmware too large)
    uint32_t              _slotSize = 0;             // bytes, half of the package area
    platform::Mutex       _mutex;                    // protects _mappedSlot and the header choice
    int                   _uploadSlot = NO_SLOT;     // slot being written (link task)
    size_t                _uploadSize = 0;           // announced by beginUpload
    size_t                _uploadNext = 0;           // offset the next chunk must have
    uint32_t              _uploadSequence = 0;       // given to the header by finishUpload
    uint8_t              *_page = nullptr;           // page buffer of uploads (pageSize, 4-byte aligned), owned
    size_t                _pageFill = 0;             // bytes waiting in _page
    int                   _mappedSlot = NO_SLOT;     // slot returned by map() (JS task)
};

} // namespace slots
