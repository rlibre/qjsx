/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The board's flash through the ROM's flash API (see flash_imxrt1062.h).
// Each operation runs from the ITCM (section .ramfunc.*, copied there at
// boot, imxrt1062.ld) with every interrupt masked: the interrupt handlers and
// FreeRTOS's tick live in the flash, which cannot be read while it erases or
// programs. The SDK's wrapper of the ROM (fsl_romapi.c) is placed in the
// ITCM too (imxrt1062.ld).
#include "flash_imxrt1062.h"

#include <string.h>

#include "fsl_device_registers.h"
#include "fsl_romapi.h"

// the board's FlexSPI configuration block (boards/<board>/flash_config.c)
extern "C" const flexspi_nor_config_t qjsxFlashConfig;

// in the ITCM, never inlined into a caller that runs from the flash
#define IN_ITCM __attribute__((section(".ramfunc.flash"), noinline))

namespace {

const uint32_t FLEXSPI_INSTANCE = 0;          // the ROM's number of FlexSPI1, the boot flash
const uint32_t XIP_BASE = 0x60000000;         // the flash in the address space

flexspi_nor_config_t norConfig;               // the ROM driver's copy of the configuration (written by it)
bool                 started = false;         // start() succeeded

/**
 * Drops what the caches and the FlexSPI's read buffers hold of
 * [offset, offset + size), after it changed. ITCM, interrupts masked.
 */
IN_ITCM void forgetCached(uint32_t offset, uint32_t size) {
    ROM_FLEXSPI_NorFlash_ClearCache(FLEXSPI_INSTANCE);
    SCB_InvalidateDCache_by_Addr((void *)(XIP_BASE + offset), (int32_t)size);
}

/**
 * Initializes the ROM driver with norConfig. ITCM: the FlexSPI is set up
 * again, the flash may not be read meanwhile.
 */
IN_ITCM status_t initializeRom() {
    uint32_t mask = DisableGlobalIRQ();
    status_t status = ROM_FLEXSPI_NorFlash_Init(FLEXSPI_INSTANCE, &norConfig);
    EnableGlobalIRQ(mask);
    return status;
}

/**
 * Erases one sector. ITCM, interrupts masked during the operation.
 */
IN_ITCM status_t eraseInItcm(uint32_t offset) {
    uint32_t mask = DisableGlobalIRQ();
    status_t status = ROM_FLEXSPI_NorFlash_Erase(FLEXSPI_INSTANCE, &norConfig, offset, flash::SECTOR_SIZE);
    forgetCached(offset, flash::SECTOR_SIZE);
    EnableGlobalIRQ(mask);
    return status;
}

/**
 * Programs one page. ITCM, interrupts masked during the operation.
 */
IN_ITCM status_t programInItcm(uint32_t offset, const uint32_t *page) {
    uint32_t mask = DisableGlobalIRQ();
    status_t status = ROM_FLEXSPI_NorFlash_ProgramPage(FLEXSPI_INSTANCE, &norConfig, offset, page);
    forgetCached(offset, flash::PAGE_SIZE);
    EnableGlobalIRQ(mask);
    return status;
}

} // namespace

namespace flash {

/**
 * 1. the configuration block, copied to RAM (the ROM driver keeps its
 *    state in it);
 * 2. the ROM driver, set up with it.
 */
bool start() {
    // the copy
    memcpy(&norConfig, &qjsxFlashConfig, sizeof(norConfig));

    // the driver
    started = initializeRom() == kStatus_Success;
    return started;
}

/**
 * Erases one sector through the ROM.
 */
bool eraseSector(uint32_t offset) {
    if (!started || offset % SECTOR_SIZE != 0) {
        return false;
    }

    return eraseInItcm(offset) == kStatus_Success;
}

/**
 * Programs one page through the ROM.
 */
bool programPage(uint32_t offset, const uint8_t *page) {
    bool aligned = offset % PAGE_SIZE == 0 && ((uintptr_t)page % sizeof(uint32_t)) == 0;
    if (!started || !aligned) {
        return false;
    }

    return programInItcm(offset, (const uint32_t *)page) == kStatus_Success;
}

/**
 * The XIP address of an offset.
 */
const uint8_t *address(uint32_t offset) {
    return (const uint8_t *)(XIP_BASE + offset);
}

} // namespace flash
