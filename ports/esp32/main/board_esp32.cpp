/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The ESP32 boards (see board.h), shared by every board: what differs comes
// from the board's description (boards/<board>/board_config.h, sdkconfig.board).
// The host link is UART0 (the boards' USB-serial bridge). The JS heap lives in
// PSRAM when the board has some, packages in the "packages" partition
// (partitions/), read in place.
#include "board.h"
#include "board_config.h"
#include "board_esp32.h"
#include "agent.h"
#include "memory.h"
#include "modules.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "sdkconfig.h"

// the board has PSRAM (its sdkconfig.board sets CONFIG_SPIRAM)
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

namespace board {

namespace {

// ### JS heap in PSRAM ########################################################

#if CONFIG_SPIRAM
const uint32_t PSRAM_CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;   // byte-addressable PSRAM

/**
 * QuickJS allocation in PSRAM.
 */
void *psramMalloc(JSMallocState *, size_t size) {
    return heap_caps_malloc(size, PSRAM_CAPS);
}

/**
 * QuickJS free (nullptr: nothing).
 */
void psramFree(JSMallocState *, void *block) {
    heap_caps_free(block);
}

/**
 * QuickJS resize, in PSRAM (size 0 frees, like realloc).
 */
void *psramRealloc(JSMallocState *, void *block, size_t size) {
    return heap_caps_realloc(block, size, PSRAM_CAPS);
}

/**
 * Real size of a block (QuickJS counts its memory with it).
 */
size_t psramUsableSize(const void *block) {
    if (block == nullptr) {
        return 0;
    }

    return heap_caps_get_allocated_size((void *)block);
}

const JSMallocFunctions psramAllocator = { &psramMalloc, &psramFree, &psramRealloc, &psramUsableSize };
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
    uint32_t magic;                  // PACKAGE_MAGIC when the slot holds a package
    uint32_t sequence;               // higher: installed later
    uint32_t size;                   // bytes of the package after the header
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
        if (_partition != nullptr) {
            return true;
        }

        _partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "packages");
        if (_partition == nullptr) {
            fprintf(stderr, "qjsx: no \"packages\" partition\n");
            return false;
        }

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
        if (_uploadSlot == NO_SLOT || offset + size > _uploadSize) {
            return false;
        }

        size_t address = slotStart(_uploadSlot) + HEADER_SIZE + offset;
        return esp_partition_write(_partition, address, data, size) == ESP_OK;
    }

    /**
     * Writes the header of the upload slot: from now on it is the installed
     * package (its sequence is the highest).
     */
    bool finishUpload(const char *buildId) override {
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
     * Maps the installed package into the address space (read in place).
     * nullptr if none; one mapping at a time.
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

        // map the package after its header
        const void *data = nullptr;
        size_t address = slotStart(slot) + HEADER_SIZE;
        esp_err_t error = esp_partition_mmap(_partition, address, header.size, ESP_PARTITION_MMAP_DATA, &data, &_mapHandle);
        if (error != ESP_OK) {
            _mutex.unlock();
            return nullptr;
        }

        _mappedSlot = slot;
        *size = header.size;
        _mutex.unlock();
        return (const uint8_t *)data;
    }

    /**
     * Unmaps what map() returned.
     */
    void unmap() {
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

FlashStorage storage;

// ### Link to the host over UART0 #############################################

const uart_port_t HOST_UART = UART_NUM_0;   // the COM port (CH343) and the IDF console
const int UART_RX_BUFFER = 4096;            // driver buffers, bytes
const int UART_TX_BUFFER = 4096;
const size_t READ_BLOCK = 256;              // bytes read at once by the reader task
const int READ_TIMEOUT_MS = 20;             // the reader gives what came after that much silence

// agent::Link over UART0. The console (printf, IDF logs) shares the wire:
// every write goes through the driver, one call at a time, so a frame is
// never cut by text, and the host takes the bytes outside frames as console.
class UartLink : public agent::Link {
public:
    /**
     * Writes whole frames (one driver call: never interleaved with a printf).
     */
    bool send(const uint8_t *data, size_t size) override {
        int written = uart_write_bytes(HOST_UART, data, size);
        return written == (int)size;
    }
};

UartLink uartLink;
agent::Agent *linkedAgent = nullptr;   // set by startLinks, before the reader starts

/**
 * Reader task of UART0: gives the received bytes to the agent, forever.
 */
void uartReader(void *) {
    // on the heap: task stacks are small
    uint8_t *buffer = (uint8_t *)memory::allocate(READ_BLOCK);

    for (;;) {
        int received = uart_read_bytes(HOST_UART, buffer, READ_BLOCK, pdMS_TO_TICKS(READ_TIMEOUT_MS));
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
 * No file system yet (the "data" partition will hold one): cwd() is "/" and
 * fs fails with ENOSYS.
 */
const char *dataDirectory() {
    return nullptr;
}

/**
 * Restarts the chip: does not return.
 */
bool restart() {
    esp_restart();
}

/**
 * The JS heap in PSRAM; without PSRAM, QuickJS's default allocator (internal
 * RAM).
 */
const JSMallocFunctions *jsAllocator() {
#if CONFIG_SPIRAM
    return &psramAllocator;
#else
    return nullptr;
#endif
}

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
 * UART0: installs its driver (the console then writes through it too),
 * attaches it to the agent for good (a serial link is always there) and
 * starts its reader task.
 */
bool startLinks(agent::Agent *agent) {
    // the driver, and stdout / stderr through it: one write call at a time
    if (uart_driver_install(HOST_UART, UART_RX_BUFFER, UART_TX_BUFFER, 0, nullptr, 0) != ESP_OK) {
        fprintf(stderr, "qjsx: cannot install the UART0 driver\n");
        return false;
    }

    uart_vfs_dev_use_driver(HOST_UART);

    // the agent talks through it from now on
    linkedAgent = agent;
    agent->attach(&uartLink);

    // the reader task
    if (!platform::startThread(&uartReader, nullptr, platform::defaultThreadOptions("agent-uart"))) {
        fprintf(stderr, "qjsx: cannot start the UART0 reader\n");
        return false;
    }

    printf("qjsx agent: listening on UART0\n");
    return true;
}

/**
 * The IDF console (UART0): the program's output when no host session is
 * open.
 */
void writeConsole(int stream, const char *text, size_t length) {
    FILE *out = stream == rt::OUTPUT_STDERR ? stderr : stdout;
    fwrite(text, 1, length, out);
    fflush(out);
}

} // namespace board
