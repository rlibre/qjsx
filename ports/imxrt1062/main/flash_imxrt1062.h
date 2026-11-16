/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The board's flash, written through the flash API of the RT1062's boot ROM
// (romapi): the ROM's code runs the operation, ours waits in the ITCM with
// every interrupt masked (nothing may run from the flash meanwhile), then
// the caches are dropped for what changed. Reading is the XIP window.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace flash {

const uint32_t SECTOR_SIZE = 4096;   // erase unit
const uint32_t PAGE_SIZE = 256;      // program unit

/**
 * Prepares the ROM's driver with the board's configuration block (copied to
 * RAM). Called once by main(), before the scheduler. False if the ROM
 * refused it.
 */
bool           start();

/**
 * Erases the sector at 'offset' (from the start of the flash, a multiple of
 * SECTOR_SIZE). About 45 ms with every interrupt masked. Any task.
 */
bool           eraseSector(uint32_t offset);

/**
 * Programs the page at 'offset' (a multiple of PAGE_SIZE) with PAGE_SIZE
 * bytes of 'page' (in RAM, 4-byte aligned). Any task.
 */
bool           programPage(uint32_t offset, const uint8_t *page);

/**
 * The address of 'offset' in the XIP window (read in place, cached).
 */
const uint8_t *address(uint32_t offset);

} // namespace flash
