/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The ESP32 boards (see board.h), shared by every board: what differs comes
// from the board's description (boards/<board>/board_config.h, sdkconfig.board).
// The host link is the console of the board: UART0 (the USB-serial bridge of
// most boards) or the USB Serial/JTAG of the chip (the M5Stack Tab5). The JS
// heap lives in PSRAM when the board has some, packages in the "packages"
// partition (partitions/), read in place.
#include "board.h"
#include "board_config.h"
#include "board_esp32.h"
#include "agent.h"
#include "memory.h"
#include "modules.h"
#include "platform.h"
#include "protocol.h"
#include "region_heap.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "sdkconfig.h"

// the measure of the memory map walks the tasks
#ifdef QJSX_MEMORY_MAP
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/freertos_debug.h"
#endif

// the host link on the USB Serial/JTAG of the chip (sdkconfig.board)
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

// the board has PSRAM (its sdkconfig.board sets CONFIG_SPIRAM)
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

namespace board {

namespace {

// ### JS heap #################################################################

// The JS heap: in PSRAM when the board has some, else in internal RAM. The
// allocator counts its blocks and keeps QuickJS's memory limit as QuickJS's
// own allocator does (board.h: the runtime measures the JS heap from it).
#if CONFIG_SPIRAM
const uint32_t JS_HEAP_CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;     // byte-addressable PSRAM
#else
const uint32_t JS_HEAP_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;   // byte-addressable internal RAM
#endif

// What the heap keeps per block besides its usable size (as QuickJS counts).
const size_t MALLOC_OVERHEAD = 8;

/**
 * Real size of a block (0 for nullptr).
 */
size_t jsHeapUsableSize(const void *block) {
    if (block == nullptr) {
        return 0;
    }

    return heap_caps_get_allocated_size((void *)block);
}

/**
 * QuickJS allocation: refused past QuickJS's memory limit, counted.
 */
void *jsHeapMalloc(JSMallocState *state, size_t size) {
    // within the limit
    if (state->malloc_size + size > state->malloc_limit) {
        return nullptr;
    }

    // the block, counted
    void *block = heap_caps_malloc(size, JS_HEAP_CAPS);
    if (block == nullptr) {
        return nullptr;
    }

    state->malloc_count++;
    state->malloc_size += jsHeapUsableSize(block) + MALLOC_OVERHEAD;
    return block;
}

/**
 * QuickJS free (nullptr: nothing), counted.
 */
void jsHeapFree(JSMallocState *state, void *block) {
    // no block: nothing to free
    if (block == nullptr) {
        return;
    }

    // uncounted, then freed
    state->malloc_count--;
    state->malloc_size -= jsHeapUsableSize(block) + MALLOC_OVERHEAD;
    heap_caps_free(block);
}

/**
 * QuickJS resize (nullptr: allocates, size 0: frees), within the limit,
 * counted.
 */
void *jsHeapRealloc(JSMallocState *state, void *block, size_t size) {
    // no block yet: an allocation
    if (block == nullptr) {
        return size == 0 ? nullptr : jsHeapMalloc(state, size);
    }

    // size 0: a free
    if (size == 0) {
        jsHeapFree(state, block);
        return nullptr;
    }

    // within the limit (malloc_size includes the old size)
    size_t oldSize = jsHeapUsableSize(block);
    if (state->malloc_size + size - oldSize > state->malloc_limit) {
        return nullptr;
    }

    // the new block, counted
    void *resized = heap_caps_realloc(block, size, JS_HEAP_CAPS);
    if (resized == nullptr) {
        return nullptr;
    }

    state->malloc_size = state->malloc_size - oldSize + jsHeapUsableSize(resized);
    return resized;
}

// QuickJS's allocator over the ESP-IDF heap (JS_HEAP_CAPS).
const JSMallocFunctions jsHeapAllocator = { &jsHeapMalloc, &jsHeapFree, &jsHeapRealloc, &jsHeapUsableSize };

// Without PSRAM the JS heap is one region of internal RAM, taken once and
// managed by memory::RegionHeap: the firmware keeps the rest, whatever the
// program does. A program that fills its region gets "out of memory"; the
// agent, the drivers and the file system still have their RAM, and the JS
// blocks no longer split the firmware's heap. QJSX_JS_HEAP_SHARED (a build
// option, to compare: idf.py -DQJSX_JS_HEAP_SHARED=ON) keeps the heap shared
// with the firmware, as before.
#if !CONFIG_SPIRAM && !defined(QJSX_JS_HEAP_SHARED)
#define JS_HEAP_REGION 1
#else
#define JS_HEAP_REGION 0
#endif

#if JS_HEAP_REGION
const size_t JS_HEAP_RESERVE = 16 * 1024;    // left to the firmware in the largest free block
const size_t JS_HEAP_SMALLEST = 64 * 1024;   // a smaller region is not worth it: shared heap

memory::RegionHeap jsRegion;                 // the region, used by the JS task only
void *jsRegionStart = nullptr;               // its first byte (for the measure of the memory map)
size_t jsRegionSize = 0;                     // its size, 0 while not reserved (JS task writes once)
bool jsRegionTried = false;                  // reserveJsRegion() ran (JS task only)

/**
 * Reserves the JS region: the largest free block of internal RAM less
 * JS_HEAP_RESERVE (the other, smaller, free blocks also stay to the
 * firmware). Called once, by the JS task at its first runtime: its stack,
 * the agent, the links and the file system are allocated before. False when
 * the block is too small: the JS heap then shares the firmware's.
 */
bool reserveJsRegion() {
    // once
    if (jsRegionTried) {
        return jsRegionSize > 0;
    }

    jsRegionTried = true;

    // the largest block, less the firmware's part
    size_t largest = heap_caps_get_largest_free_block(JS_HEAP_CAPS);
    if (largest < JS_HEAP_RESERVE + JS_HEAP_SMALLEST) {
        printf("qjsx: no region for the JS heap (largest free block %u KB): it shares the RAM\n",
            (unsigned)(largest / 1024));
        return false;
    }

    // the region, then the heap in it
    size_t size = largest - JS_HEAP_RESERVE;
    void *region = heap_caps_malloc(size, JS_HEAP_CAPS);
    bool reserved = region != nullptr && jsRegion.init(region, size);
    if (!reserved) {
        heap_caps_free(region);
        printf("qjsx: cannot reserve the JS heap (%u KB): it shares the RAM\n", (unsigned)(size / 1024));
        return false;
    }

    jsRegionStart = region;
    jsRegionSize = size;
    printf("qjsx: JS heap of %u KB\n", (unsigned)(size / 1024));
    return true;
}

#endif

// ### Package storage #########################################################

// The "packages" partition holds two slots. Each starts with a header sector,
// followed by the package. An upload goes into the slot that is not
// installed, and its header is written last: an interrupted upload leaves the
// installed package as it was, and the running program (mapped from its
// slot) is never erased under it.
const uint32_t PACKAGE_MAGIC = 0x4b504a45;   // "EJPK" in little endian
const size_t   HEADER_SIZE = 4096;           // one flash sector, erased with the slot
const size_t   SECTOR_SIZE = 4096;           // flash erase unit
const size_t   BUILD_ID_SIZE = 64;           // as the agent's build ids
const int      NO_SLOT = -1;                 // none (installed, uploaded or mapped)

// Header of a slot, at its start.
struct SlotHeader {
    uint32_t magic;                   // PACKAGE_MAGIC when the slot holds a package
    uint32_t sequence;                // higher: installed later
    uint32_t size;                    // bytes of the package after the header
    char     buildId[BUILD_ID_SIZE];  // zero-terminated
};

// agent::Storage on the "packages" partition. Called on the transport task
// (uploads) and on the JS task (openPackage, installed): _mutex protects the
// slot numbers.
class FlashStorage : public agent::Storage {
public:
    /**
     * Finds the partition (once). False if partitions.csv has none.
     */
    bool open() {
        // found already
        if (_partition != nullptr) {
            return true;
        }

        // the partition, of our own data subtype 0x40 (the tables of partitions/)
        _partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "packages");
        if (_partition == nullptr) {
            fprintf(stderr, "qjsx: no \"packages\" partition\n");
            return false;
        }

        // two slots of half
        _slotSize = _partition->size / 2;
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
        bool fits = _partition != nullptr && size > 0 && size <= _slotSize - HEADER_SIZE;
        bool free = target != _mappedSlot;
        if (!fits || !free) {
            _mutex.unlock();
            return false;
        }

        // erase the header and the room of the package: the slot is now empty
        size_t erased = (HEADER_SIZE + size + SECTOR_SIZE - 1) / SECTOR_SIZE * SECTOR_SIZE;
        bool ok = esp_partition_erase_range(_partition, slotStart(target), erased) == ESP_OK;

        // remember the upload: where, how big, which sequence it will take
        _uploadSlot = ok ? target : NO_SLOT;
        _uploadSize = size;
        _uploadSequence = installedSlot == NO_SLOT ? 1 : installedHeader.sequence + 1;
        _mutex.unlock();
        return ok;
    }

    /**
     * Writes a chunk after the header of the upload slot.
     */
    bool writeUpload(size_t offset, const uint8_t *data, size_t size) override {
        // an upload going on, the chunk within the announced size
        bool uploading = _uploadSlot != NO_SLOT;
        bool fits = offset + size <= _uploadSize;
        if (!uploading || !fits) {
            return false;
        }

        // at its place after the header
        size_t address = slotStart(_uploadSlot) + HEADER_SIZE + offset;
        return esp_partition_write(_partition, address, data, size) == ESP_OK;
    }

    /**
     * Writes the header of the upload slot: from now on it is the installed
     * package (its sequence is the highest).
     */
    bool finishUpload(const char *buildId) override {
        // no upload going on
        if (_uploadSlot == NO_SLOT) {
            return false;
        }

        // the header, build id truncated to its field
        SlotHeader header = {};
        header.magic = PACKAGE_MAGIC;
        header.sequence = _uploadSequence;
        header.size = (uint32_t)_uploadSize;
        strncpy(header.buildId, buildId, BUILD_ID_SIZE - 1);

        // written last: until now the slot had no valid header
        bool ok = esp_partition_write(_partition, slotStart(_uploadSlot), &header, sizeof(header)) == ESP_OK;
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
        // the installed slot
        SlotHeader header;
        _mutex.lock();
        int slot = findInstalled(&header);
        _mutex.unlock();
        if (slot == NO_SLOT) {
            return false;
        }

        // what its header says
        snprintf(buildId, buildIdSize, "%s", header.buildId);
        *packageSize = header.size;
        return true;
    }

    /**
     * Maps the installed package into the address space (read in place).
     * nullptr if none; one mapping at a time.
     */
    const uint8_t *map(size_t *size) {
        _mutex.lock();

        // the installed slot, if any and if none is mapped already
        SlotHeader header;
        int slot = findInstalled(&header);
        bool mappedAlready = _mappedSlot != NO_SLOT;
        if (slot == NO_SLOT || mappedAlready) {
            _mutex.unlock();
            return nullptr;
        }

        // map the package after its header
        const void *data = nullptr;
        size_t address = slotStart(slot) + HEADER_SIZE;
        esp_err_t error = esp_partition_mmap(_partition, address, header.size, ESP_PARTITION_MMAP_DATA, &data, &_mapHandle);
        if (error != ESP_OK) {
            _mutex.unlock();
            return nullptr;
        }

        // mapped: this slot is never erased until unmap()
        _mappedSlot = slot;
        *size = header.size;
        _mutex.unlock();
        return (const uint8_t *)data;
    }

    /**
     * Unmaps what map() returned.
     */
    void unmap() {
        // only if a slot is mapped
        _mutex.lock();
        if (_mappedSlot != NO_SLOT) {
            esp_partition_munmap(_mapHandle);
            _mappedSlot = NO_SLOT;
        }

        _mutex.unlock();
    }

private:
    /**
     * Offset of a slot in the partition.
     */
    size_t slotStart(int slot) const {
        return (size_t)slot * _slotSize;
    }

    /**
     * Reads the header of a slot; true if it holds a package.
     */
    bool readHeader(int slot, SlotHeader *header) {
        // the header from the flash
        if (esp_partition_read(_partition, slotStart(slot), header, sizeof(SlotHeader)) != ESP_OK) {
            return false;
        }

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
        // no partition: no slot
        if (_partition == nullptr) {
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

    const esp_partition_t       *_partition = nullptr;    // "packages"; nullptr until open()
    size_t                       _slotSize = 0;           // bytes, half of the partition
    platform::Mutex              _mutex;                  // protects _mappedSlot and the header choice
    int                          _uploadSlot = NO_SLOT;   // slot being written (transport task)
    size_t                       _uploadSize = 0;         // announced by beginUpload
    uint32_t                     _uploadSequence = 0;     // given to the header by finishUpload
    int                          _mappedSlot = NO_SLOT;   // slot mapped by map() (JS task)
    esp_partition_mmap_handle_t  _mapHandle = 0;          // of that mapping
};

FlashStorage storage;   // the package storage of the board

// ### Files of the programs: LittleFS on the "data" partition #################

const char DATA_FOLDER[] = "/data";        // where the partition is mounted: process.cwd()
const char DATA_PARTITION[] = "data";      // its label in partitions/*.csv
bool dataMounted = false;                  // set once by mountData() (app_main, before the JS task)

// ### Link to the host ########################################################
// The console of the board (sdkconfig): UART0, or the USB Serial/JTAG of the
// chip when the board's sdkconfig.board puts the console there
// (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG: the M5Stack Tab5, whose USB-C port
// reaches the chip with no bridge). Both have a driver with buffers, read
// by a task of ours and written by the agent; printf goes through the same
// driver.

const int    LINK_RX_BUFFER = 4096;      // receive buffer of the driver, bytes
const int    LINK_TX_BUFFER = 4096;      // transmit buffer of the driver, bytes
const size_t READ_BLOCK = 256;           // bytes read at once by the reader task
const int    READ_TIMEOUT_MS = 20;       // the reader gives what came after that much silence

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG

const char LINK_NAME[] = "USB Serial/JTAG";   // in the message of startLinks
const int  LINK_WRITE_TIMEOUT_MS = 100;       // a host that reads nothing: the rest of a write is dropped

/**
 * Installs the driver of the USB Serial/JTAG; stdout / stderr go through it.
 */
bool installLink() {
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.rx_buffer_size = LINK_RX_BUFFER;
    config.tx_buffer_size = LINK_TX_BUFFER;
    if (usb_serial_jtag_driver_install(&config) != ESP_OK) {
        return false;
    }

    usb_serial_jtag_vfs_use_driver();
    return true;
}

/**
 * Writes bytes to the host; returns how many went into the driver (fewer
 * when the host has read nothing for LINK_WRITE_TIMEOUT_MS: USB is not a
 * wire that always takes the bytes).
 */
int writeLink(const uint8_t *data, size_t size) {
    return usb_serial_jtag_write_bytes(data, size, pdMS_TO_TICKS(LINK_WRITE_TIMEOUT_MS));
}

/**
 * Reads what came, at most 'size' bytes, waiting at most 'timeoutMs'.
 */
int readLink(uint8_t *buffer, size_t size, int timeoutMs) {
    return usb_serial_jtag_read_bytes(buffer, size, pdMS_TO_TICKS(timeoutMs));
}

/**
 * Waits until what was written has left, at most 'timeoutMs'.
 */
void waitLinkSent(int timeoutMs) {
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(timeoutMs));
}

#else

const char        LINK_NAME[] = "UART0";   // in the message of startLinks
const uart_port_t HOST_UART = UART_NUM_0;  // the COM port (CH343...) and the IDF console

/**
 * Installs the driver of UART0; stdout / stderr go through it.
 */
bool installLink() {
    if (uart_driver_install(HOST_UART, LINK_RX_BUFFER, LINK_TX_BUFFER, 0, nullptr, 0) != ESP_OK) {
        return false;
    }

    uart_vfs_dev_use_driver(HOST_UART);
    return true;
}

/**
 * Writes bytes to the host (one driver call: never interleaved with a
 * printf); returns how many.
 */
int writeLink(const uint8_t *data, size_t size) {
    return uart_write_bytes(HOST_UART, data, size);
}

/**
 * Reads what came, at most 'size' bytes, waiting at most 'timeoutMs'.
 */
int readLink(uint8_t *buffer, size_t size, int timeoutMs) {
    return uart_read_bytes(HOST_UART, buffer, size, pdMS_TO_TICKS(timeoutMs));
}

/**
 * Waits until what was written has left, at most 'timeoutMs'.
 */
void waitLinkSent(int timeoutMs) {
    uart_wait_tx_done(HOST_UART, pdMS_TO_TICKS(timeoutMs));
}

#endif

// agent::Link over the console. The console (printf, IDF logs) shares the
// link: every write goes through the driver, one call at a time, so a frame
// is never cut by text, and the host takes the bytes outside frames as
// console.
class ConsoleLink : public agent::Link {
public:
    /**
     * Writes whole frames (one driver call).
     */
    bool send(const uint8_t *data, size_t size) override {
        int written = writeLink(data, size);
        return written == (int)size;
    }
};

ConsoleLink consoleLink;               // the agent's link
agent::Agent *linkedAgent = nullptr;   // set by startLinks, before the reader starts

/**
 * Reader task of the link: gives the received bytes to the agent, forever.
 */
void linkReader(void *) {
    // on the heap: task stacks are small
    uint8_t *buffer = (uint8_t *)memory::allocate(READ_BLOCK);

    for (;;) {
        // what came, given after READ_TIMEOUT_MS of silence or a full block
        int received = readLink(buffer, READ_BLOCK, READ_TIMEOUT_MS);
        if (received > 0) {
            linkedAgent->receive(buffer, (size_t)received);
        }
    }
}

} // namespace

/**
 * The chip (IDF target), the board's name (board_config.h); flash and PSRAM
 * sizes read from the chip.
 */
void identity(Identity *identity) {
    // the flash chip's size (0 if it cannot be read)
    uint32_t flashSize = 0;
    if (esp_flash_get_size(nullptr, &flashSize) != ESP_OK) {
        flashSize = 0;
    }

    // the PSRAM's size (0 without PSRAM)
    uint32_t psramSize = 0;
#if CONFIG_SPIRAM
    psramSize = (uint32_t)esp_psram_get_size();
#endif

    *identity = Identity{ CONFIG_IDF_TARGET, BOARD_NAME, flashSize, psramSize };
}

/**
 * process.platform on the ESP32 devices.
 */
const char *platformName() {
    return "esp32";
}

/**
 * The programs reach only /data: the VFS of ESP-IDF also holds the UART of
 * the agent's link (/dev/uart/0) and the console (user, 2026-11-13).
 */
bool confineFiles() {
    return true;
}

/**
 * "/data" once mountData() mounted the "data" partition; nullptr if it could
 * not (cwd() is then "/" and fs fails with ENOSYS).
 */
const char *dataDirectory() {
    return dataMounted ? DATA_FOLDER : nullptr;
}

/**
 * The size of the LittleFS of the "data" partition; 0 when not mounted.
 */
uint64_t dataCapacity() {
    // from LittleFS, if mounted
    size_t total = 0;
    size_t used = 0;
    if (!dataMounted || esp_littlefs_info(DATA_PARTITION, &total, &used) != ESP_OK) {
        return 0;
    }

    return total;
}

/**
 * Mounts the LittleFS of the "data" partition on /data, formatting it the
 * first time (or when it does not hold a valid LittleFS). False after
 * printing why: no partition, or the flash refused.
 */
bool mountData() {
    // the partition on DATA_FOLDER, formatted when it cannot be mounted
    esp_vfs_littlefs_conf_t settings = {};
    settings.base_path = DATA_FOLDER;
    settings.partition_label = DATA_PARTITION;
    settings.format_if_mount_failed = true;
    esp_err_t result = esp_vfs_littlefs_register(&settings);
    if (result != ESP_OK) {
        printf("qjsx: the \"%s\" partition cannot be mounted on %s (%s): no files for the programs\n", DATA_PARTITION,
               DATA_FOLDER, esp_err_to_name(result));
        return false;
    }

    dataMounted = true;
    return true;
}

/**
 * Restarts the chip: does not return. What was written was only copied into
 * the transmit buffers of the drivers (the answer to RESET, the last words of
 * a program to a UART or to the USB host): wait until it has left, for at
 * most RESTART_FLUSH_MS in all, else it would be lost.
 */
bool restart() {
    // one deadline for every output: the agent's link, the open UARTs, the
    // USB serial functions
    int64_t deadline = platform::monotonicMs() + RESTART_FLUSH_MS;
    waitLinkSent(RESTART_FLUSH_MS);
    hal::serial::drainUntil(deadline);
#if QJSX_HAS_USB
    hal::usb::drainUntil(deadline);
#endif

    esp_restart();
}

/**
 * The base MAC of the chip (eFuses): 6 bytes.
 */
size_t chipId(uint8_t *out) {
    esp_read_mac(out, ESP_MAC_BASE);
    return 6;
}

/**
 * The MAC addresses ESP-IDF derives from the base one; false for a kind the
 * chip does not have (esp_read_mac refuses it).
 */
bool macAddress(MacKind kind, uint8_t *out) {
    // the ESP-IDF type of the kind (base for any other)
    esp_mac_type_t type = ESP_MAC_BASE;
    if (kind == MacKind::wifi) {
        type = ESP_MAC_WIFI_STA;
    } else if (kind == MacKind::ap) {
        type = ESP_MAC_WIFI_SOFTAP;
    } else if (kind == MacKind::bluetooth) {
        type = ESP_MAC_BT;
    } else if (kind == MacKind::ethernet) {
        type = ESP_MAC_ETH;
    }

    // derived from the base MAC by ESP-IDF
    return esp_read_mac(out, type) == ESP_OK;
}

/**
 * The JS heap: in PSRAM (ESP-IDF's heap, counted, see jsHeapMalloc); without
 * PSRAM in the region of internal RAM reserved now, or in the firmware's
 * heap when it could not be reserved. Called once, by the JS task.
 */
const JSMallocFunctions *jsAllocator() {
#if JS_HEAP_REGION
    // without PSRAM: the region, if it can be reserved
    if (reserveJsRegion()) {
        return memory::jsAllocator(&jsRegion, false);
    }
#endif

    // else the ESP-IDF heap
    return &jsHeapAllocator;
}

/**
 * The size of the JS region, once reserved; 0 when the heap shares the RAM
 * or is in PSRAM.
 */
size_t jsHeapLimit() {
#if JS_HEAP_REGION
    if (jsRegionSize > 0) {
        memory::HeapStats stats = {};
        jsRegion.stats(&stats);
        return stats.total;
    }
#endif

    return 0;
}

/**
 * 64 KB with PSRAM; without it, a small one: the receive buffer of the
 * agent and the copies of a message being sent take internal RAM.
 */
size_t agentMaxMessage() {
#if CONFIG_SPIRAM
    return agent::DEFAULT_MAX_MESSAGE;
#else
    return agent::SMALL_MAX_MESSAGE;
#endif
}

/**
 * Free internal RAM (and PSRAM when the board has some), from the ESP-IDF
 * heaps.
 */
bool memoryStatus(MemoryStatus *out) {
    // internal RAM: now, the largest block, the lowest since the boot
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    out->ramFree = heap_caps_get_free_size(internal);
    out->ramLargest = heap_caps_get_largest_free_block(internal);
    out->ramLowest = heap_caps_get_minimum_free_size(internal);

    // PSRAM
#if CONFIG_SPIRAM
    out->psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
#else
    out->psramFree = 0;
    out->psramLargest = 0;
#endif

    // the region of the JS heap (without PSRAM)
#if JS_HEAP_REGION
    out->jsHeapSize = jsRegionSize;
#else
    out->jsHeapSize = 0;
#endif
    return true;
}

#ifdef QJSX_MEMORY_MAP
/**
 * Measure (temporary): the heaps of internal RAM (summary, then every
 * block), the tasks (stack block, its size, bytes never used) and the JS
 * region (address, size, RegionHeap's counts).
 */
void printMemoryMap() {
    // the heaps: summary and blocks
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    heap_caps_dump(MALLOC_CAP_INTERNAL);

    // every task
    TaskIterator_t iterator = {};

    while (xTaskGetNext(&iterator) != -1) {
        TaskHandle_t task = iterator.pxTaskHandle;
        uint8_t *stack = pxTaskGetStackStart(task);
        printf("MAP task %s stack %p size %u unused %u\n", pcTaskGetName(task), stack,
            (unsigned)heap_caps_get_allocated_size(stack), (unsigned)uxTaskGetStackHighWaterMark(task));
    }

    // the JS region
#if JS_HEAP_REGION
    memory::HeapStats stats = {};
    jsRegion.stats(&stats);
    printf("MAP region at %p size %u total %u used %u peak %u largest free %u used blocks %u free blocks %u\n",
        jsRegionStart, (unsigned)jsRegionSize, (unsigned)stats.total, (unsigned)stats.used, (unsigned)stats.peak,
        (unsigned)stats.largestFree, (unsigned)stats.usedBlocks, (unsigned)stats.freeBlocks);
#endif
}

#endif

/**
 * Below the stack of the JS task (main.cpp).
 */
size_t jsStackSize() {
    return JS_STACK_LIMIT;
}

/**
 * The two slots of the "packages" partition (found on the first call).
 */
agent::Storage *packageStorage() {
    storage.open();
    return &storage;
}

/**
 * The installed package, mapped from flash (read in place).
 */
const uint8_t *openPackage(size_t *size) {
    if (!storage.open()) {
        return nullptr;
    }

    return storage.map(size);
}

/**
 * Unmaps the package.
 */
void closePackage(const uint8_t *) {
    storage.unmap();
}

/**
 * The console (UART0 or USB Serial/JTAG): installs its driver (the console
 * then writes through it too), attaches it to the agent for good (a serial
 * link is always there) and starts its reader task.
 */
bool startLinks(agent::Agent *agent) {
    // the driver, and stdout / stderr through it: one write call at a time
    if (!installLink()) {
        fprintf(stderr, "qjsx: cannot install the driver of %s\n", LINK_NAME);
        return false;
    }

    // the agent talks through it from now on
    linkedAgent = agent;
    agent->attach(&consoleLink);

    // the reader task
    if (!platform::startThread(&linkReader, nullptr, platform::defaultThreadOptions("agent-link"))) {
        fprintf(stderr, "qjsx: cannot start the reader of %s\n", LINK_NAME);
        return false;
    }

    printf("qjsx agent: listening on %s\n", LINK_NAME);
    return true;
}

/**
 * The IDF console (UART0 or USB Serial/JTAG): the program's output when no
 * host session is open.
 */
void writeConsole(int stream, const char *text, size_t length) {
    FILE *out = stream == rt::OUTPUT_STDERR ? stderr : stdout;
    fwrite(text, 1, length, out);
    fflush(out);
}

} // namespace board
