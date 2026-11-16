/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/* The FlexSPI configuration block (FCB) of the Teensy 4.0's flash, a Winbond
 * W25Q16JV (2 MB, QSPI): the boot ROM of the i.MX RT1062 reads it at the very
 * start of the flash (0x60000000) to set the FlexSPI up before it reads the
 * image (reference manual, "Serial NOR configuration block"). The firmware
 * also gives a RAM copy of it to the ROM's flash API (flash_imxrt1062.cpp).
 *
 * The lookup table holds the command sequences the ROM and the FlexSPI use:
 * read (quad I/O fast read 0xEB: the XIP window), read status, write enable,
 * sector erase (4 KB), block erase (64 KB), page program (256 bytes), chip
 * erase. The flash's quad mode (QE bit) is set by PJRC: the board already
 * boots in quad mode. */
#include "fsl_romapi.h"

/* one LUT sequence: four instructions, two per word */
#define LUT_SEQUENCE_WORDS 4

/* index of an instruction pair in the lookup table */
#define LUT_INDEX(sequence, pair) ((sequence) * LUT_SEQUENCE_WORDS + (pair))

__attribute__((section(".flash_config"), used))
const flexspi_nor_config_t qjsxFlashConfig = {
    .memConfig = {
        .tag = FLEXSPI_CFG_BLK_TAG,
        .version = FLEXSPI_CFG_BLK_VERSION,
        .readSampleClkSrc = kFLEXSPIReadSampleClk_LoopbackFromDqsPad,
        .csHoldTime = 3,
        .csSetupTime = 3,
        .deviceType = kFLEXSPIDeviceType_SerialNOR,
        .sflashPadType = kSerialFlash_4Pads,
        .serialClkFreq = kFLEXSPISerialClk_100MHz,
        .sflashA1Size = 2 * 1024 * 1024,
        .lookupTable = {
            /* read: quad I/O fast read, 24-bit address on 4 pads, 6 dummy
               cycles (2 mode + 4 wait) */
            [LUT_INDEX(NOR_CMD_LUT_SEQ_IDX_READ, 0)] =
                FSL_ROM_FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0xEB, RADDR_SDR, FLEXSPI_4PAD, 0x18),
            [LUT_INDEX(NOR_CMD_LUT_SEQ_IDX_READ, 1)] =
                FSL_ROM_FLEXSPI_LUT_SEQ(DUMMY_SDR, FLEXSPI_4PAD, 0x06, READ_SDR, FLEXSPI_4PAD, 0x04),

            /* read status register 1 */
            [LUT_INDEX(NOR_CMD_LUT_SEQ_IDX_READSTATUS, 0)] =
                FSL_ROM_FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0x05, READ_SDR, FLEXSPI_1PAD, 0x04),

            /* write enable */
            [LUT_INDEX(NOR_CMD_LUT_SEQ_IDX_WRITEENABLE, 0)] =
                FSL_ROM_FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0x06, STOP, FLEXSPI_1PAD, 0),

            /* sector erase, 4 KB */
            [LUT_INDEX(NOR_CMD_LUT_SEQ_IDX_ERASESECTOR, 0)] =
                FSL_ROM_FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0x20, RADDR_SDR, FLEXSPI_1PAD, 0x18),

            /* read JEDEC id */
            [LUT_INDEX(NOR_CMD_LUT_SEQ_IDX_READID, 0)] =
                FSL_ROM_FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0x9F, READ_SDR, FLEXSPI_1PAD, 0x04),

            /* block erase, 64 KB */
            [LUT_INDEX(NOR_CMD_LUT_SEQ_IDX_ERASEBLOCK, 0)] =
                FSL_ROM_FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0xD8, RADDR_SDR, FLEXSPI_1PAD, 0x18),

            /* page program, 256 bytes, single line */
            [LUT_INDEX(NOR_CMD_LUT_SEQ_IDX_PAGEPROGRAM, 0)] =
                FSL_ROM_FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0x02, RADDR_SDR, FLEXSPI_1PAD, 0x18),
            [LUT_INDEX(NOR_CMD_LUT_SEQ_IDX_PAGEPROGRAM, 1)] =
                FSL_ROM_FLEXSPI_LUT_SEQ(WRITE_SDR, FLEXSPI_1PAD, 0x04, STOP, FLEXSPI_1PAD, 0),

            /* chip erase */
            [LUT_INDEX(NOR_CMD_LUT_SEQ_IDX_CHIPERASE, 0)] =
                FSL_ROM_FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0x60, STOP, FLEXSPI_1PAD, 0),
        },
    },
    .pageSize = 256,
    .sectorSize = 4 * 1024,
    .ipcmdSerialClkFreq = kFLEXSPISerialClk_30MHz,
    .blockSize = 64 * 1024,
    .isUniformBlockSize = 0,
};
