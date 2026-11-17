/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The i.MX RT1062 boards (see board.h), shared by every board: what differs
// comes from the board's description (boards/<board>/board_config.h). The
// host link is the chip's USB port (CDC, usb_serial_imxrt1062.cpp), which
// also carries the console. Packages live in two slots of the flash
// (ports/freertos/slot_storage.*), read in place through the XIP window. The
// JS heap is a RegionHeap over its region of the OCRAM (imxrt1062.ld).
#include "board.h"
#include "board_config.h"
#include "board_imxrt1062.h"
#include "flash_imxrt1062.h"
#include "usb_serial_imxrt1062.h"
#include "slot_storage.h"
#include "agent.h"
#include "memory.h"
#include "platform.h"
#include "protocol.h"
#include "region_heap.h"

#include <malloc.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "fsl_clock.h"
#include "fsl_device_registers.h"

// from imxrt1062.ld: the regions of the OCRAM and the end of the firmware
extern "C" uint8_t __qjsx_js_heap_start[];
extern "C" uint8_t __qjsx_js_heap_end[];
extern "C" uint8_t __qjsx_c_heap_start[];
extern "C" uint8_t __qjsx_c_heap_end[];
extern "C" const uint8_t __qjsx_image_end[];

// from syscalls_imxrt1062.cpp: the end of the C heap taken so far
extern "C" uint8_t *qjsxHeapBreak();

namespace board {

namespace {

// ### JS heap #################################################################

memory::RegionHeap jsHeap;    // over its region of the OCRAM, JS task only
size_t jsHeapSize = 0;        // bytes of the region (startMemory)

// ### Package storage #########################################################

// The package area of the flash and how to write it (flash_imxrt1062.h).
const slots::Flash packageFlash = {
    PACKAGES_OFFSET,
    PACKAGES_SIZE,
    flash::SECTOR_SIZE,
    flash::PAGE_SIZE,
    &flash::eraseSector,
    &flash::programPage,
    &flash::address,
};

slots::SlotStorage storage;   // the two slots of the package area

/**
 * Opens the storage (once): the firmware must end before it.
 */
bool openStorage() {
    return storage.open(&packageFlash, (uintptr_t)__qjsx_image_end);
}

// ### Link to the host over USB ###############################################

const size_t   READ_BLOCK = 512;          // bytes read at once by the reader task
const uint32_t READ_TIMEOUT_MS = 20;      // the reader looks again after that much silence

// agent::Link over the USB serial port; the console shares it (each write
// holds the port's write lock: a frame is never cut by text).
class UsbLink : public agent::Link {
public:
    /**
     * Writes whole frames.
     */
    bool send(const uint8_t *data, size_t size) override {
        return usbserial::write(data, size);
    }
};

UsbLink       usbLink;                 // the agent's link (startLinks)
agent::Agent *linkedAgent = nullptr;   // set by startLinks, before the reader starts

/**
 * Reader task of the USB link: gives the received bytes to the agent,
 * forever. Woken by the USB interrupt, and every READ_TIMEOUT_MS in case a
 * wake-up was missed.
 */
void usbReader(void *) {
    // on the heap: task stacks are small
    uint8_t *buffer = (uint8_t *)memory::allocate(READ_BLOCK);

    for (;;) {
        // bytes arrived, or READ_TIMEOUT_MS passed
        usbserial::waitReceived(READ_TIMEOUT_MS);

        // take everything there is
        for (;;) {
            size_t got = usbserial::read(buffer, READ_BLOCK);
            if (got == 0) {
                break;
            }

            linkedAgent->receive(buffer, got);
        }
    }
}

// ### Lowest free C heap ######################################################

size_t lowestFree = SIZE_MAX;   // lowest free C heap memoryStatus() saw

/**
 * Free bytes of the C heap: what malloc holds free plus what it never took.
 */
size_t freeCHeap() {
    struct mallinfo info = mallinfo();
    size_t neverTaken = (size_t)(__qjsx_c_heap_end - qjsxHeapBreak());
    return (size_t)info.fordblks + neverTaken;
}

} // namespace

/**
 * The JS heap over its region (one JS task: no lock).
 */
void startMemory() {
    jsHeapSize = (size_t)(__qjsx_js_heap_end - __qjsx_js_heap_start);
    jsHeap.init(__qjsx_js_heap_start, jsHeapSize);
}

/**
 * The chip, the board's name (board_config.h), its flash, no PSRAM.
 */
void identity(Identity *identity) {
    *identity = Identity{ "i.MX RT1062", BOARD_NAME, FLASH_SIZE, 0 };
}

/**
 * process.platform on the i.MX RT1062 devices.
 */
const char *platformName() {
    return "imxrt1062";
}

/**
 * No file system yet (/data later, LittleFS): nothing to confine.
 */
bool confineFiles() {
    return false;
}

/**
 * No file system yet: cwd() is "/" and fs fails with ENOSYS.
 */
const char *dataDirectory() {
    return nullptr;
}

/**
 * No file system yet.
 */
uint64_t dataCapacity() {
    return 0;
}

/**
 * Restarts the chip: does not return. The answer to RESET was only queued for
 * the USB: let it leave first (RESTART_FLUSH_MS at most). The host then sees
 * the USB port go away and come back.
 */
bool restart() {
    // the pending output to the USB first
    usbserial::flush(RESTART_FLUSH_MS);

    // then the reset; no return statement: NVIC_SystemReset() is declared
    // __NO_RETURN by CMSIS (it waits for the reset in a loop itself)
    NVIC_SystemReset();
}

/**
 * The chip's unique id: the two words of the OCOTP's UID fuses (shadow
 * registers CFG0 and CFG1), high word first, big endian: 8 bytes.
 */
size_t chipId(uint8_t *out) {
    // the two fuse words, high word first
    CLOCK_EnableClock(kCLOCK_Ocotp);
    uint32_t words[2] = { OCOTP->CFG1, OCOTP->CFG0 };

    // their bytes, most significant first
    for (int i = 0; i < 8; i++) {
        uint32_t word = words[i / 4];
        out[i] = (uint8_t)(word >> (24 - 8 * (i % 4)));
    }

    return 8;
}

/**
 * The MAC address in the OCOTP's MAC fuses (PJRC programs one per board),
 * for "base" and "ethernet" (the chip has an Ethernet controller); false for
 * the other kinds, and when the fuses are blank.
 */
bool macAddress(MacKind kind, uint8_t *out) {
    bool fromFuses = kind == MacKind::base || kind == MacKind::ethernet;
    if (!fromFuses) {
        return false;
    }

    // MAC1 holds the two high bytes, MAC0 the four low ones
    CLOCK_EnableClock(kCLOCK_Ocotp);
    uint32_t high = OCOTP->MAC1 & 0xFFFF;
    uint32_t low = OCOTP->MAC0;
    bool blank = high == 0 && low == 0;
    if (blank) {
        return false;
    }

    // the six bytes, most significant first
    out[0] = (uint8_t)(high >> 8);
    out[1] = (uint8_t)high;
    out[2] = (uint8_t)(low >> 24);
    out[3] = (uint8_t)(low >> 16);
    out[4] = (uint8_t)(low >> 8);
    out[5] = (uint8_t)low;
    return true;
}

/**
 * The JS heap in its region (one JS task: no lock).
 */
const JSMallocFunctions *jsAllocator() {
    return memory::jsAllocator(&jsHeap, false);
}

/**
 * The bytes the region can hold: QuickJS's memory limit.
 */
size_t jsHeapLimit() {
    memory::HeapStats stats = {};
    jsHeap.stats(&stats);
    return stats.total;
}

/**
 * A small one, as the boards without PSRAM: the agent's buffers take the C
 * heap.
 */
size_t agentMaxMessage() {
    return agent::SMALL_MAX_MESSAGE;
}

/**
 * The C heap's free bytes (now, the end never taken as the largest block
 * it can surely give, the lowest memoryStatus() saw), no PSRAM, the JS
 * region's size.
 */
bool memoryStatus(MemoryStatus *out) {
    // free now, and the lowest seen so far
    size_t now = freeCHeap();
    lowestFree = now < lowestFree ? now : lowestFree;

    // the C heap, no PSRAM, the JS region
    out->ramFree = now;
    out->ramLargest = (size_t)(__qjsx_c_heap_end - qjsxHeapBreak());
    out->ramLowest = lowestFree;
    out->psramFree = 0;
    out->psramLargest = 0;
    out->jsHeapSize = jsHeapSize;
    return true;
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
    openStorage();
    return &storage;
}

/**
 * The installed package, read in place from the flash.
 */
const uint8_t *openPackage(size_t *size) {
    if (!openStorage()) {
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
 * The USB link (started by main): attached to the agent for good, its reader
 * task.
 */
bool startLinks(agent::Agent *agent) {
    // the agent talks through it from now on
    linkedAgent = agent;
    agent->attach(&usbLink);

    // the reader task
    platform::ThreadOptions options = platform::defaultThreadOptions("agent-usb");
    if (!platform::startThread(&usbReader, nullptr, options)) {
        fprintf(stderr, "qjsx: cannot start the USB reader\n");
        return false;
    }

    printf("qjsx agent: listening on USB\n");
    return true;
}

/**
 * The USB console: the program's output when no host session is open.
 */
void writeConsole(int, const char *text, size_t length) {
    usbserial::write((const uint8_t *)text, length);
}

} // namespace board
