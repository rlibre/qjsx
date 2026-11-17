/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/* Linker script of the qjsx firmware on the i.MX RT1062 (Teensy 4.0), derived
 * from the MCUXpresso SDK's MIMXRT1062xxxxx_flexspi_nor.ld (BSD-3-Clause,
 * NXP). Memory map agreed 2026-11-16 (CLAUDE.md), with the factory FlexRAM
 * split (fuses):
 *
 *   flash  0x60000000  the firmware area (1 MB): FlexSPI configuration block,
 *                      IVT + boot data, vectors, code, read-only data, the
 *                      load images of .data and of the code copied to ITCM
 *   ITCM   0x00000000  128 KB: code copied at boot (CodeQuickAccess and the
 *                      .ramfunc.* sections: hot engine functions, flash writes)
 *   DTCM   0x20000000  128 KB: .data, .bss (USB buffers: not cached, safe for
 *                      the USB DMA), the main stack (interrupts)
 *   OCRAM  0x20200000  768 KB: the JS heap (QJSX_JS_HEAP_SIZE), then the C
 *                      heap (_sbrk: FreeRTOS tasks and stacks, memory::allocate)
 *
 * The SDK's startup (startup_MIMXRT1062.S) copies from __etext, one after the
 * other: .data, .ram_function, .ncache.init; their load images must follow
 * each other in this order. */

ENTRY(Reset_Handler)

/* the main stack: interrupts, and main() until the scheduler starts */
MAIN_STACK_SIZE = 8K;

/* the JS heap, at the start of the OCRAM (board_imxrt1062.cpp) */
QJSX_JS_HEAP_SIZE = 512K;

MEMORY
{
  FLASH_CONFIG  (RX)  : ORIGIN = 0x60000000, LENGTH = 4K
  IVT           (RX)  : ORIGIN = 0x60001000, LENGTH = 4K
  VECTORS       (RX)  : ORIGIN = 0x60002000, LENGTH = 1K
  FLASH         (RX)  : ORIGIN = 0x60002400, LENGTH = 1M - 0x2400
  ITCM          (RX)  : ORIGIN = 0x00000000, LENGTH = 128K
  DTCM          (RW)  : ORIGIN = 0x20000000, LENGTH = 128K
  OCRAM         (RW)  : ORIGIN = 0x20200000, LENGTH = 768K
}

SECTIONS
{
  __qjsx_flash_start = ORIGIN(FLASH_CONFIG);

  /* the FlexSPI configuration block (boards/<board>/flash_config.c) */
  .flash_config :
  {
    KEEP(*(.flash_config))
  } > FLASH_CONFIG

  /* the image vector table and the boot data (boot_header.c) */
  .ivt :
  {
    KEEP(*(.ivt))
    . = ALIGN(16);
    KEEP(*(.boot_data))
  } > IVT

  /* the vector table (startup_MIMXRT1062.S) */
  .interrupts :
  {
    __VECTOR_TABLE = .;
    __Vectors = .;
    KEEP(*(.isr_vector))
  } > VECTORS

  /* code and read-only data, run in place; not the SDK's wrapper of the
     ROM's flash API, which must run from the ITCM (flash_imxrt1062.cpp) */
  .text :
  {
    . = ALIGN(4);
    *(EXCLUDE_FILE(*fsl_romapi.c.obj *region_heap.cpp.obj *libc_a-memset.o) .text)
    *(EXCLUDE_FILE(*fsl_romapi.c.obj *region_heap.cpp.obj *libc_a-memset.o) .text*)
    *(.rodata)
    *(.rodata*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
  } > FLASH

  .ARM.extab :
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } > FLASH

  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } > FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } > FLASH

  .init_array :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
  } > FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } > FLASH

  /* the template of the thread_local variables (tls_imxrt1062.cpp copies it
     into each task's block): .tdata, then .tbss, which takes no room */
  .tdata : ALIGN(8)
  {
    __tdata_start = .;
    *(.tdata .tdata.* .gnu.linkonce.td.*)
    . = ALIGN(8);
    __tdata_end = .;
  } > FLASH

  .tbss : ALIGN(8)
  {
    __tbss_start = .;
    *(.tbss .tbss.* .gnu.linkonce.tb.*)
    *(.tcommon)
    . = ALIGN(8);
    __tbss_end = .;
  } > FLASH

  /* the load images start here (startup_MIMXRT1062.S) */
  . = ALIGN(4);
  __etext = .;

  /* initialized data, copied to the DTCM */
  .data : AT(__etext)
  {
    . = ALIGN(4);
    __data_start__ = .;
    *(.data)
    *(.data*)
    *(DataQuickAccess)
    . = ALIGN(4);
    __data_end__ = .;
  } > DTCM

  /* code copied to the ITCM: the SDK's CodeQuickAccess and the sections
     renamed .ramfunc.* (CMakeLists.txt) */
  __ram_function_flash_start = __etext + (__data_end__ - __data_start__);
  .ram_function : AT(__ram_function_flash_start)
  {
    . = ALIGN(4);
    __ram_function_start__ = .;
    *(CodeQuickAccess)
    *(.ramfunc*)
    *fsl_romapi.c.obj(.text .text*)
    /* EXPERIMENT 2026-11-17 (await on the Teensy): the JS heap and memset */
    *region_heap.cpp.obj(.text .text*)
    *libc_a-memset.o(.text .text*)
    . = ALIGN(4);
    __ram_function_end__ = .;
  } > ITCM

  /* data marked NonCacheable by the SDK: in the DTCM, which has no cache */
  __ncache_flash_start = __ram_function_flash_start + (__ram_function_end__ - __ram_function_start__);
  .ncache.init : AT(__ncache_flash_start)
  {
    . = ALIGN(4);
    __noncachedata_start__ = .;
    *(NonCacheable.init)
    . = ALIGN(4);
    __noncachedata_init_end__ = .;
  } > DTCM

  .ncache (NOLOAD) :
  {
    *(NonCacheable)
    . = ALIGN(4);
    __noncachedata_end__ = .;
  } > DTCM

  /* the end of the image in the flash (boot data), checked against the
     package area at boot (board_imxrt1062.cpp) */
  __qjsx_image_end = __ncache_flash_start + (__noncachedata_init_end__ - __noncachedata_start__);
  __qjsx_image_size = __qjsx_image_end - __qjsx_flash_start;
  ASSERT(__qjsx_image_end <= ORIGIN(FLASH) + LENGTH(FLASH), "the firmware does not fit in its 1 MB")

  /* zero-initialized data */
  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    __bss_start__ = .;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end__ = .;
  } > DTCM

  /* the main stack, at the end of the DTCM */
  __StackTop = ORIGIN(DTCM) + LENGTH(DTCM);
  __StackLimit = __StackTop - MAIN_STACK_SIZE;
  PROVIDE(__stack = __StackTop);
  ASSERT(__bss_end__ <= __StackLimit, "the DTCM overflowed: .data + .bss + main stack")

  /* the JS heap, then the C heap, in the OCRAM */
  .js_heap (NOLOAD) :
  {
    . = ALIGN(8);
    __qjsx_js_heap_start = .;
    . += QJSX_JS_HEAP_SIZE;
    __qjsx_js_heap_end = .;
  } > OCRAM

  __qjsx_c_heap_start = ALIGN(__qjsx_js_heap_end, 8);
  __qjsx_c_heap_end = ORIGIN(OCRAM) + LENGTH(OCRAM);

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
