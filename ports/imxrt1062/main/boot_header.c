/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/* The image vector table (IVT) and the boot data of the firmware, at
 * 0x60001000 (imxrt1062.ld): after the FlexSPI configuration block of the
 * board (boards/<board>/flash_config.c), the boot ROM reads them to know
 * where the image is and where it starts (reference manual, "Program image").
 * The image runs in place from the flash (no copy by the ROM), no device
 * configuration data (DCD), no signature (CSF). */
#include <stdint.h>

/* the IVT header: tag 0xD1, length 0x0020 (big endian), version 0x41 */
#define IVT_HEADER 0x412000D1u

/* "no plugin" in the boot data */
#define IVT_PLUGIN_NONE 0u

/* from imxrt1062.ld: the vector table (the image's entry: the ROM takes the
   stack pointer and the reset handler from it), the start of the flash and
   the size of the image (an absolute symbol: its address is the value) */
extern const uint32_t __isr_vector[];
extern const uint8_t __qjsx_flash_start[];
extern const uint8_t __qjsx_image_size[];

/* The boot data: where the image is and how big. */
typedef struct {
    uint32_t start;         /* address of the image: the start of the flash */
    uint32_t size;          /* bytes of the image */
    uint32_t plugin;        /* IVT_PLUGIN_NONE */
    uint32_t placeholder;   /* unused, all ones */
} BootData;

/* The image vector table. */
typedef struct {
    uint32_t header;        /* IVT_HEADER */
    uint32_t entry;         /* the vector table */
    uint32_t reserved1;     /* 0 */
    uint32_t dcd;           /* device configuration data: none */
    uint32_t bootData;      /* &qjsxBootData */
    uint32_t self;          /* &qjsxImageVectorTable */
    uint32_t csf;           /* signature: none */
    uint32_t reserved2;     /* 0 */
} ImageVectorTable;

__attribute__((section(".boot_data"), used))
const BootData qjsxBootData = {
    .start = (uint32_t)__qjsx_flash_start,
    .size = (uint32_t)__qjsx_image_size,
    .plugin = IVT_PLUGIN_NONE,
    .placeholder = 0xFFFFFFFFu,
};

__attribute__((section(".ivt"), used))
const ImageVectorTable qjsxImageVectorTable = {
    .header = IVT_HEADER,
    .entry = (uint32_t)__isr_vector,
    .reserved1 = 0,
    .dcd = 0,
    .bootData = (uint32_t)&qjsxBootData,
    .self = (uint32_t)&qjsxImageVectorTable,
    .csf = 0,
    .reserved2 = 0,
};
