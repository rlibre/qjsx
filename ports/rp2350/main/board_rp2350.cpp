/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The RP2350 boards (see board.h), shared by every board: what differs comes
// from the board's description (boards/<board>/board_config.h, board.cmake).
// The host link is the chip's own USB port (CDC, through the pico-sdk's
// stdio_usb), which also carries the console. Packages live in two slots of
// the flash (board_config.h), read in place through the XIP window. The JS
// heap is a RegionHeap over the PSRAM when the board has one.
#include "board.h"
#include "board_config.h"
#include "board_rp2350.h"
#include "agent.h"
#include "memory.h"
#include "modules.h"
#include "platform.h"
#include "psram_rp2350.h"
#include "region_heap.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "pico/flash.h"
#include "pico/sem.h"
#include "pico/stdio.h"

// end of the firmware in the flash (address), from the pico-sdk's linker script
extern "C" char __flash_binary_end;

// 1: the JS heap stays in SRAM, the PSRAM unused (CMake option, for measures)
#ifndef QJSX_JS_HEAP_IN_SRAM
#define QJSX_JS_HEAP_IN_SRAM 0
#endif

namespace board {

namespace {

// ### JS heap in PSRAM ########################################################

memory::RegionHeap psramHeap;   // over the whole PSRAM, JS task only
bool psramReady = false;        // startMemory() set it up

// ### Flash operations ########################################################

// Erasing or programming the flash stops the XIP window: no code may run
// from the flash meanwhile, on either core. flash_safe_execute() holds the
// other core in RAM and masks the interrupts of this one, then runs the
// operation from RAM. One sector at a time, so the USB interrupts are served
// between two sectors.
const uint32_t FLASH_TIMEOUT_MS = 1000;   // to hold the other core

// One erase (data nullptr) or program, run by flash_safe_execute().
struct FlashOperation {
    uint32_t       offset;   // from the start of the flash
    const uint8_t *data;     // in RAM (never flash nor PSRAM); nullptr: erase
    size_t         size;     // bytes: a multiple of the sector (erase) or page (program)
};

/**
 * Runs a FlashOperation (from RAM, other core held, interrupts masked).
 */
void __no_inline_not_in_flash_func(runFlashOperation)(void *parameter) {
    FlashOperation *operation = (FlashOperation *)parameter;
    if (operation->data == nullptr) {
        flash_range_erase(operation->offset, operation->size);
    } else {
        flash_range_program(operation->offset, operation->data, operation->size);
    }
}

/**
 * Erases one sector (FLASH_SECTOR_SIZE) at 'offset'.
 */
bool eraseSector(uint32_t offset) {
    FlashOperation operation = { offset, nullptr, FLASH_SECTOR_SIZE };
    return flash_safe_execute(&runFlashOperation, &operation, FLASH_TIMEOUT_MS) == PICO_OK;
}

/**
 * Programs one page (FLASH_PAGE_SIZE bytes of 'page', in RAM) at 'offset'.
 */
bool programPage(uint32_t offset, const uint8_t *page) {
    FlashOperation operation = { offset, page, FLASH_PAGE_SIZE };
    return flash_safe_execute(&runFlashOperation, &operation, FLASH_TIMEOUT_MS) == PICO_OK;
}

// ### Package storage #########################################################

// The package area holds two slots. Each starts with a header sector,
// followed by the package. An upload goes into the slot that is not
// installed, and its header is written last: an interrupted upload leaves the
// installed package as it was, and the running program (read in place from
// its slot) is never erased under it. Same layout as the ESP32 port.
const uint32_t PACKAGE_MAGIC = 0x4b504a45;   // "EJPK" in little endian
const uint32_t HEADER_SIZE = 4096;           // one flash sector, erased with the slot
const size_t   BUILD_ID_SIZE = 64;           // as the agent's build ids
const int      NO_SLOT = -1;                 // none (installed, uploaded or mapped)

// Header of a slot, at its start.
struct SlotHeader {
    uint32_t magic;                   // PACKAGE_MAGIC when the slot holds a package
    uint32_t sequence;                // higher: installed later
    uint32_t size;                    // bytes of the package after the header
    char     buildId[BUILD_ID_SIZE];  // zero-terminated
};

static_assert(sizeof(SlotHeader) <= FLASH_PAGE_SIZE, "a slot header is written as one page");

// agent::Storage on the package area of the flash. Called on the link task
// (uploads) and on the JS task (openPackage, installed): _mutex protects the
// slot numbers. The flash is programmed by pages: the upload goes through a
// page buffer, in order.
class FlashStorage : public agent::Storage {
public:
    /**
     * Checks the package area (once): the firmware must end before it.
     * False otherwise, after saying so.
     */
    bool open() {
        if (_opened) {
            return true;
        }

        // the firmware must not reach the package area
        uintptr_t firmwareEnd = (uintptr_t)&__flash_binary_end;
        uintptr_t areaStart = XIP_BASE + PACKAGES_OFFSET;
        if (firmwareEnd > areaStart) {
            fprintf(stderr, "qjsx: the firmware overlaps the package area (board_config.h)\n");
            return false;
        }

        // the page buffer of uploads, in RAM for good
        _page = (uint8_t *)memory::allocate(FLASH_PAGE_SIZE);
        _slotSize = PACKAGES_SIZE / 2;
        _opened = true;
        return true;
    }

    /**
     * Erases the slot that is neither installed nor running, for a package
     * of 'size' bytes. False if it does not fit, or if both slots are busy
     * (a program still runs from the slot an upload would need).
     */
    bool beginUpload(size_t size) override {
        _mutex.lock();

        // the target: the slot that is not installed
        SlotHeader installedHeader;
        int installedSlot = findInstalled(&installedHeader);
        int target = installedSlot == 0 ? 1 : 0;

        // it must hold the package and not be the one the program runs from
        bool fits = _opened && size > 0 && size <= _slotSize - HEADER_SIZE;
        bool free = target != _mappedSlot;
        if (!fits || !free) {
            _mutex.unlock();
            return false;
        }

        // erase the header and the room of the package, one sector at a time:
        // the slot is now empty
        uint32_t erased = (HEADER_SIZE + (uint32_t)size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE * FLASH_SECTOR_SIZE;
        bool ok = true;
        for (uint32_t done = 0; done < erased && ok; done += FLASH_SECTOR_SIZE) {
            ok = eraseSector(slotOffset(target) + done);
        }

        // remember the upload: where, how big, which sequence it will take
        _uploadSlot = ok ? target : NO_SLOT;
        _uploadSize = size;
        _uploadNext = 0;
        _pageFill = 0;
        _uploadSequence = installedSlot == NO_SLOT ? 1 : installedHeader.sequence + 1;
        _mutex.unlock();
        return ok;
    }

    /**
     * Writes a chunk after the header of the upload slot. The chunks come in
     * order; full pages are programmed, the rest waits in the page buffer.
     */
    bool writeUpload(size_t offset, const uint8_t *data, size_t size) override {
        bool inOrder = offset == _uploadNext;
        if (_uploadSlot == NO_SLOT || !inOrder || offset + size > _uploadSize) {
            return false;
        }

        // fill the page buffer, program it each time it is full
        while (size > 0) {
            size_t room = FLASH_PAGE_SIZE - _pageFill;
            size_t taken = size < room ? size : room;
            memcpy(_page + _pageFill, data, taken);
            _pageFill += taken;
            _uploadNext += taken;
            data += taken;
            size -= taken;

            if (_pageFill == FLASH_PAGE_SIZE && !flushPage()) {
                return false;
            }
        }

        return true;
    }

    /**
     * Programs what is left of the package, then the header of the upload
     * slot: from now on it is the installed package (its sequence is the
     * highest).
     */
    bool finishUpload(const char *buildId) override {
        if (_uploadSlot == NO_SLOT || _uploadNext != _uploadSize) {
            return false;
        }

        // the last, partial page of the package
        if (_pageFill > 0 && !flushPage()) {
            _uploadSlot = NO_SLOT;
            return false;
        }

        // the header, build id truncated to its field
        SlotHeader header = {};
        header.magic = PACKAGE_MAGIC;
        header.sequence = _uploadSequence;
        header.size = (uint32_t)_uploadSize;
        strncpy(header.buildId, buildId, BUILD_ID_SIZE - 1);

        // written last, as one page: until now the slot had no valid header
        memset(_page, 0xff, FLASH_PAGE_SIZE);
        memcpy(_page, &header, sizeof(header));
        bool ok = programPage(slotOffset(_uploadSlot), _page);
        _uploadSlot = NO_SLOT;
        return ok;
    }

    /**
     * Drops the upload: its slot has no header, the installed one stays.
     */
    void abortUpload() override {
        _uploadSlot = NO_SLOT;
    }

    /**
     * Build id and size of the installed package. False if none.
     */
    bool installed(char *buildId, size_t buildIdSize, size_t *packageSize) override {
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
     * The installed package in the XIP window (read in place), nullptr if
     * none; one at a time (its slot is then never erased).
     */
    const uint8_t *map(size_t *size) {
        _mutex.lock();

        // the installed slot, if any
        SlotHeader header;
        int slot = findInstalled(&header);
        if (slot == NO_SLOT || _mappedSlot != NO_SLOT) {
            _mutex.unlock();
            return nullptr;
        }

        // the package after its header
        _mappedSlot = slot;
        *size = header.size;
        const uint8_t *package = flashAddress(slotOffset(slot) + HEADER_SIZE);
        _mutex.unlock();
        return package;
    }

    /**
     * Forgets what map() returned: its slot may be erased again.
     */
    void unmap() {
        _mutex.lock();
        _mappedSlot = NO_SLOT;
        _mutex.unlock();
    }

private:
    /**
     * Offset of a slot from the start of the flash.
     */
    uint32_t slotOffset(int slot) const {
        return PACKAGES_OFFSET + (uint32_t)slot * _slotSize;
    }

    /**
     * Address of a flash offset in the XIP window (cached; the pico-sdk
     * flushes the cache after each erase and program).
     */
    static const uint8_t *flashAddress(uint32_t offset) {
        return (const uint8_t *)(XIP_BASE + offset);
    }

    /**
     * Programs the page buffer at its place in the upload slot (padded with
     * 0xff, the erased value), then empties it.
     */
    bool flushPage() {
        // the page the buffer ends: its data started _pageFill bytes earlier
        size_t pageStart = _uploadNext - _pageFill;
        memset(_page + _pageFill, 0xff, FLASH_PAGE_SIZE - _pageFill);
        uint32_t offset = slotOffset(_uploadSlot) + HEADER_SIZE + (uint32_t)pageStart;

        _pageFill = 0;
        return programPage(offset, _page);
    }

    /**
     * Reads the header of a slot; true if it holds a package.
     */
    bool readHeader(int slot, SlotHeader *header) {
        memcpy(header, flashAddress(slotOffset(slot)), sizeof(SlotHeader));

        // a written header, a size that fits, a terminated build id
        bool written = header->magic == PACKAGE_MAGIC;
        bool fits = header->size > 0 && header->size <= _slotSize - HEADER_SIZE;
        bool terminated = memchr(header->buildId, 0, BUILD_ID_SIZE) != nullptr;
        return written && fits && terminated;
    }

    /**
     * The installed slot (the valid one with the highest sequence) and its
     * header, or NO_SLOT. _mutex held.
     */
    int findInstalled(SlotHeader *header) {
        if (!_opened) {
            return NO_SLOT;
        }

        // read both headers
        SlotHeader first;
        SlotHeader second;
        bool firstValid = readHeader(0, &first);
        bool secondValid = readHeader(1, &second);

        // the valid one, or the later of two
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

    bool            _opened = false;          // open() succeeded
    uint32_t        _slotSize = 0;            // bytes, half of the package area
    platform::Mutex _mutex;                   // protects _mappedSlot and the header choice
    int             _uploadSlot = NO_SLOT;    // slot being written (link task)
    size_t          _uploadSize = 0;          // announced by beginUpload
    size_t          _uploadNext = 0;          // offset the next chunk must have
    uint32_t        _uploadSequence = 0;      // given to the header by finishUpload
    uint8_t        *_page = nullptr;          // page buffer of uploads (FLASH_PAGE_SIZE), owned
    size_t          _pageFill = 0;            // bytes waiting in _page
    int             _mappedSlot = NO_SLOT;    // slot returned by map() (JS task)
};

FlashStorage storage;

// ### Link to the host over USB ###############################################

const size_t READ_BLOCK = 256;            // bytes read at once by the reader task
const uint32_t READ_TIMEOUT_MS = 20;      // the reader looks again after that much silence
const uint32_t RESTART_FLUSH_MS = 100;    // restart() lets the USB send the answer that long

// agent::Link over the USB CDC port. The console (printf) shares it: every
// write goes through stdio_put_string(), which holds the stdout lock for the
// whole call, so a frame is never cut by text, and the host takes the bytes
// outside frames as console.
class UsbLink : public agent::Link {
public:
    /**
     * Writes whole frames (one stdio call, no newline translation).
     */
    bool send(const uint8_t *data, size_t size) override {
        int written = stdio_put_string((const char *)data, (int)size, false, false);
        return written == (int)size;
    }
};

UsbLink usbLink;
agent::Agent *linkedAgent = nullptr;   // set by startLinks, before the reader starts
semaphore_t received;                  // released by the USB stack when bytes arrive

/**
 * Called by the USB stack (interrupt or task) when bytes arrive: wakes the
 * reader.
 */
void onCharsAvailable(void *) {
    sem_release(&received);
}

/**
 * Reader task of the USB link: gives the received bytes to the agent,
 * forever. Woken by onCharsAvailable(), and every READ_TIMEOUT_MS in case a
 * wake-up was missed.
 */
void usbReader(void *) {
    // on the heap: task stacks are small
    uint8_t *buffer = (uint8_t *)memory::allocate(READ_BLOCK);

    for (;;) {
        sem_acquire_timeout_ms(&received, READ_TIMEOUT_MS);

        // take everything there is, without waiting
        for (;;) {
            int got = stdio_get_until((char *)buffer, (int)READ_BLOCK, get_absolute_time());
            if (got <= 0) {
                break;
            }

            linkedAgent->receive(buffer, (size_t)got);
        }
    }
}

} // namespace

/**
 * 1. the PSRAM, if the board has one (and the build does not keep the JS heap
 *    in SRAM for a measure: QJSX_JS_HEAP_IN_SRAM);
 * 2. the heap over all of it.
 */
void startMemory() {
#if QJSX_JS_HEAP_IN_SRAM
    return;
#endif

    if (PSRAM_SIZE == 0) {
        return;
    }

    // the chip
    bool answered = startPsram(PSRAM_CS_PIN, PSRAM_MAX_CLOCK_HZ, PSRAM_SIZE);
    if (!answered) {
        return;
    }

    // the heap
    psramReady = psramHeap.init((void *)PSRAM_WINDOW, PSRAM_SIZE);
}

/**
 * The chip (RP2350A or B, from the pico-sdk board), the board's name
 * (board_config.h), the flash size of the board, the PSRAM in use (0 if
 * none or if it did not answer).
 */
void identity(Identity *identity) {
#if PICO_RP2350A
    const char *chip = "RP2350A";
#else
    const char *chip = "RP2350B";
#endif

    uint32_t psramSize = psramReady ? (uint32_t)PSRAM_SIZE : 0;
    *identity = Identity{ chip, BOARD_NAME, (uint32_t)PICO_FLASH_SIZE_BYTES, psramSize };
}

/**
 * process.platform on the RP2350 devices.
 */
const char *platformName() {
    return "rp2350";
}

/**
 * A real board: its programs see only its own file system.
 */
bool confineFiles() {
    return false;
}

/**
 * No file system yet (the microSD card later): cwd() is "/" and fs fails with
 * ENOSYS.
 */
const char *dataDirectory() {
    return nullptr;
}

/**
 * Restarts the chip: does not return. The answer to RESET was only queued
 * for the USB: let it leave first. The host then sees the USB port go away
 * and come back.
 */
bool restart() {
    stdio_flush();
    vTaskDelay(pdMS_TO_TICKS(RESTART_FLUSH_MS));
    watchdog_reboot(0, 0, 0);

    for (;;) {
        tight_loop_contents();
    }
}

/**
 * The JS heap in the PSRAM (one JS task: no lock); without PSRAM, QuickJS's
 * default allocator (SRAM).
 */
const JSMallocFunctions *jsAllocator() {
    if (!psramReady) {
        return nullptr;
    }

    return memory::jsAllocator(&psramHeap, false);
}

/**
 * Below the stack of the JS task (main.cpp).
 */
size_t jsStackSize() {
    return JS_STACK_LIMIT;
}

/**
 * The two slots of the package area (checked on the first call).
 */
agent::Storage *packageStorage() {
    storage.open();
    return &storage;
}

/**
 * The installed package, read in place from the flash.
 */
const uint8_t *openPackage(size_t *size) {
    if (!storage.open()) {
        return nullptr;
    }

    return storage.map(size);
}

/**
 * Releases the package's slot.
 */
void closePackage(const uint8_t *) {
    storage.unmap();
}

/**
 * The USB link (stdio_usb, started by main): attached to the agent for good
 * (the port stays, even when no host has it open), its reader task on core 0.
 */
bool startLinks(agent::Agent *agent) {
    // the wake-up of the reader, set before the USB stack may call it
    sem_init(&received, 0, 1);
    stdio_set_chars_available_callback(&onCharsAvailable, nullptr);

    // the agent talks through it from now on
    linkedAgent = agent;
    agent->attach(&usbLink);

    // the reader task
    platform::ThreadOptions options = platform::defaultThreadOptions("agent-usb");
    options.core = LINK_TASK_CORE;
    if (!platform::startThread(&usbReader, nullptr, options)) {
        fprintf(stderr, "qjsx: cannot start the USB reader\n");
        return false;
    }

    printf("qjsx agent: listening on USB\n");
    if (PSRAM_SIZE > 0 && !psramReady && !QJSX_JS_HEAP_IN_SRAM) {
        printf("qjsx: the PSRAM did not answer: JS heap in SRAM\n");
    }

    return true;
}

/**
 * The USB console: the program's output when no host session is open.
 */
void writeConsole(int stream, const char *text, size_t length) {
    FILE *out = stream == rt::OUTPUT_STDERR ? stderr : stdout;
    fwrite(text, 1, length, out);
    fflush(out);
}

} // namespace board
