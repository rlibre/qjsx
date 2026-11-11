/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What the common code needs from a board: the chip and its wiring (memory,
// flash, links to the host). One implementation per port:
//   host/platform/board_host.cpp     the desktop agent ("qjsx agent")
//   ports/esp32/...                  ESP32-S3 (to come)
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
 * Restarts the chip: does not return on a target. Returns false when the
 * board cannot restart (desktop): the caller restarts the program instead.
 */
bool restart();

/**
 * Allocator of the JS runtime (in PSRAM on the targets), or nullptr for the
 * default one of QuickJS.
 */
const JSMallocFunctions *jsAllocator();

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
