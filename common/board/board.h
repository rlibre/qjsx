/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What the common code needs from a board: the chip and its wiring (memory,
// flash, links to the host). One implementation per port:
//   host/platform/board_host.cpp     the desktop agent ("qjsx agent")
//   ports/esp32/main/board_esp32.cpp every ESP32 board (boards/<board>/ describes it)
//   ports/rp2350/...                 RP2350 (to come)
// The operating system services (threads, locks, time) are in platform.h.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"

namespace agent {
class Agent;
class Storage;
}

namespace board {

struct Identity {
    const char *chip;        // "ESP32-S3", "RP2350B", "desktop"
    const char *board;       // "Olimex PICO2-XXL"...
    uint32_t    flashSize;   // bytes, 0 if unknown
    uint32_t    psramSize;   // bytes, 0 if none
};

/**
 * Fills *identity.
 */
void identity(Identity *identity);

/**
 * Value of process.platform: Node's names on the desktop ("win32", "darwin",
 * "linux"), the port's own on a target. A static string.
 */
const char *platformName();

/**
 * Root of the program's files when the agent runs it: process.cwd(), and what
 * fs resolves relative paths against. Absolute, '/' separators, static or
 * owned by the board. nullptr if the board has no file system.
 */
const char *dataDirectory();

/**
 * True if the programs must reach no file outside dataDirectory(): the
 * desktop agent and the twin, which run on the PC's own disk, and the
 * boards, whose VFS also holds devices (the UART of the agent's link: user,
 * 2026-11-13). A device has no other permissions.
 */
bool confineFiles();

/**
 * Bytes of the storage of dataDirectory() (the "data" partition of a board),
 * reported by INFO; 0 when unknown (the desktop: the PC's disk) or without
 * file system.
 */
uint64_t dataCapacity();

/**
 * Restarts the chip: does not return on a target, whose pending output (the
 * agent's link, its open UARTs, its USB serial functions) is sent first, for
 * at most RESTART_FLUSH_MS in all. Returns false when the board cannot
 * restart (desktop): the caller restarts the program instead.
 */
bool restart();

const int RESTART_FLUSH_MS = 500;   // what restart() waits at most for the pending output

const size_t MAX_CHIP_ID = 16;      // bytes of the longest chip id (chipId)

/**
 * The unique id of the chip, its bytes in 'out' (MAX_CHIP_ID at most); returns
 * how many. ESP32: its base MAC (6 bytes, eFuses); the desktop agent and the
 * twin: 6 bytes kept in their storage, drawn at random the first time.
 */
size_t chipId(uint8_t *out);

// The MAC addresses of a chip, all derived from its base MAC (as ESP-IDF).
enum class MacKind {
    base,        // the base MAC (eFuses)
    wifi,        // WiFi station
    ap,          // WiFi access point
    bluetooth,   // Bluetooth
    ethernet,    // Ethernet
};

/**
 * The MAC address of 'kind' in 'out' (6 bytes). False when the chip has none
 * of that kind.
 */
bool macAddress(MacKind kind, uint8_t *out);

/**
 * Allocator of the JS runtime (in PSRAM on the targets), or nullptr for the
 * default one of QuickJS. It counts its blocks in the JSMallocState
 * (malloc_count, malloc_size) and keeps QuickJS's memory limit, as QuickJS's
 * own allocator does: the runtime measures the JS heap from malloc_size.
 */
const JSMallocFunctions *jsAllocator();

/**
 * Largest message of the agent's link, both ways, in bytes (at least
 * agent::MIN_MAX_MESSAGE): the agent's receive buffer is sized by it, its
 * answers and events stay below it, the host learns it from HELLO and
 * splits what it sends. A board without PSRAM keeps it small.
 */
size_t agentMaxMessage();

// What memoryStatus() reports, in bytes.
struct MemoryStatus {
    size_t ramFree;        // free internal RAM now
    size_t ramLargest;     // largest free block of internal RAM
    size_t ramLowest;      // lowest free internal RAM since the boot
    size_t psramFree;      // free PSRAM now (0 without PSRAM)
    size_t psramLargest;   // largest free block of PSRAM (0 without PSRAM)
    size_t jsHeapSize;     // region reserved for the JS heap (0: it shares the RAM, or the PSRAM)
};

/**
 * The free memory of the board in 'out'. False when the board cannot tell
 * (the desktop: the PC's memory says nothing about a device).
 */
bool memoryStatus(MemoryStatus *out);

#ifdef QJSX_MEMORY_MAP
/**
 * Measure (temporary, QJSX_MEMORY_MAP): prints on the console the heaps of
 * the board, every block of them, the tasks with their stacks and the
 * region of the JS heap. Called by the JS task (the global __memoryMap()).
 */
void printMemoryMap();
#endif

/**
 * Bytes of stack the JS code may use on the JS thread (QuickJS turns going
 * past it into a RangeError instead of a crash): a bit less than the stack of
 * the task the port created for it. 0: QuickJS's default (desktop, large
 * stacks).
 */
size_t jsStackSize();

/**
 * Where the agent writes the uploaded packages (a flash partition on a
 * target, a file on the desktop).
 */
agent::Storage *packageStorage();

/**
 * The installed package, readable in place until closePackage() (mapped
 * flash on a target, the file read in memory on the desktop), or nullptr
 * if none is installed.
 */
const uint8_t *openPackage(size_t *size);

/**
 * Releases what openPackage() returned (unmaps the flash, frees the file
 * copy).
 */
void           closePackage(const uint8_t *package);

/**
 * Starts the links to the host (USB, UART, TCP...): the bytes they receive
 * go to agent->receive(), from their own threads. Returns false if no link
 * could start (after printing why).
 */
bool startLinks(agent::Agent *agent);

/**
 * Console output when no host session is open (UART, USB, stdout).
 */
void writeConsole(int stream, const char *text, size_t length);

} // namespace board
