#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# qjsx firmware for the i.MX RT1062 boards (NXP MCUXpresso SDK + FreeRTOS, one
# Cortex-M7 core). One firmware per board: the board is chosen with
# QJSX_BOARD (a folder of boards/), each board in its own build folder.
# tools/teensy.ps1 runs CMake with the paths of what is installed:
#   cmake -G Ninja -B build-teensy40 -D QJSX_BOARD=teensy40
#         -D CMAKE_TOOLCHAIN_FILE=toolchain.cmake -D ARM_TOOLCHAIN_PATH=...
#         -D MCUXSDK_PATH=... -D FREERTOS_KERNEL_PATH=...
# The SDK's own CMake / Kconfig machinery is not used: the few files of the
# SDK the firmware needs are listed below. The sources shared by every device
# port are in ports/cmake/qjsx.cmake; main/ is the code shared by every
# RT1062 board; boards/<board>/ holds what differs (board_config.h, the
# flash's configuration block).
cmake_minimum_required(VERSION 3.13)

project(qjsx C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# the board
set(QJSX_BOARD "teensy40" CACHE STRING "board folder in ports/imxrt1062/boards")
set(QJSX_BOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/boards/${QJSX_BOARD}")
if(NOT EXISTS "${QJSX_BOARD_DIR}/board_config.h")
    message(FATAL_ERROR "qjsx: unknown board '${QJSX_BOARD}' (no boards/${QJSX_BOARD}/board_config.h)")
endif()

# the SDK and FreeRTOS (paths given by tools/teensy.ps1)
if(NOT MCUXSDK_PATH OR NOT FREERTOS_KERNEL_PATH)
    message(FATAL_ERROR "qjsx: MCUXSDK_PATH and FREERTOS_KERNEL_PATH are required (see tools/teensy.ps1)")
endif()

# the sources shared by every device port
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/qjsx.cmake")
set(MAIN_DIR "${CMAKE_CURRENT_LIST_DIR}/main")

# ### The SDK #################################################################

set(DEVICE_DIR "${MCUXSDK_PATH}/devices/RT/RT1060/MIMXRT1062")
set(USB_DIR "${MCUXSDK_PATH}/middleware/usb")
set(SDK_SOURCES
    # the chip: start-up, system, clocks, the ROM's flash API
    "${DEVICE_DIR}/gcc/startup_MIMXRT1062.S"
    "${DEVICE_DIR}/system_MIMXRT1062.c"
    "${DEVICE_DIR}/drivers/fsl_clock.c"
    "${DEVICE_DIR}/drivers/romapi/fsl_romapi.c"
    # drivers: common, PIT (time, the loader), TRNG (random bytes)
    "${MCUXSDK_PATH}/drivers/common/fsl_common.c"
    "${MCUXSDK_PATH}/drivers/common/fsl_common_arm.c"
    "${MCUXSDK_PATH}/drivers/pit/fsl_pit.c"
    "${MCUXSDK_PATH}/drivers/trng/fsl_trng.c"
    # the USB device stack (EHCI controller, CDC ACM class, PHY)
    "${USB_DIR}/device/usb_device_dci.c"
    "${USB_DIR}/device/usb_device_ehci.c"
    "${USB_DIR}/device/usb_device_ch9.c"
    "${USB_DIR}/device/class/usb_device_class.c"
    "${USB_DIR}/device/class/usb_device_cdc_acm.c"
    "${USB_DIR}/phy/usb_phy.c")
set(SDK_INCLUDE_DIRS
    "${DEVICE_DIR}"
    "${DEVICE_DIR}/drivers"
    "${DEVICE_DIR}/drivers/romapi"
    "${MCUXSDK_PATH}/devices/RT/RT1060/periph"
    "${MCUXSDK_PATH}/drivers/common"
    "${MCUXSDK_PATH}/drivers/pit"
    "${MCUXSDK_PATH}/drivers/trng"
    "${MCUXSDK_PATH}/arch/arm/CMSIS/Core/Include"
    "${USB_DIR}/include"
    "${USB_DIR}/device"
    "${USB_DIR}/device/class"
    "${USB_DIR}/phy")
set(SDK_DEFINES
    CPU_MIMXRT1062DVL6A
    # the start-up copies .data, the ITCM code and the non-cached data, then
    # clears .bss
    __STARTUP_INITIALIZE_RAMFUNCTION
    __STARTUP_INITIALIZE_NONCACHEDATA
    __STARTUP_CLEAR_BSS
    # the image runs from the flash; our own boot header (main/boot_header.c)
    XIP_EXTERNAL_FLASH=1
    XIP_BOOT_HEADER_ENABLE=0)

# ### FreeRTOS ################################################################

set(FREERTOS_PORT_DIR "${FREERTOS_KERNEL_PATH}/portable/GCC/ARM_CM4F")
set(FREERTOS_SOURCES
    "${FREERTOS_KERNEL_PATH}/tasks.c"
    "${FREERTOS_KERNEL_PATH}/queue.c"
    "${FREERTOS_KERNEL_PATH}/list.c"
    "${FREERTOS_KERNEL_PATH}/timers.c"
    "${FREERTOS_PORT_DIR}/port.c"
    "${FREERTOS_KERNEL_PATH}/portable/MemMang/heap_3.c")

# ### The engine ##############################################################

# in its own library: its "list.h" (QuickJS's lists) would hide FreeRTOS's in
# the firmware's sources. The newlib compat folder (pthread.h, dirent.h) comes
# before the system headers. A board without PSRAM: the small-heap settings.
add_library(qjsx-engine-target STATIC ${QJSX_ENGINE_SOURCES})
target_include_directories(qjsx-engine-target PRIVATE "${QJSX_NEWLIB_COMPAT_DIR}" "${QJSX_ROOT}/engine/qjsx")
target_compile_definitions(qjsx-engine-target PRIVATE ${QJSX_DEFINES} ${QJSX_SMALL_HEAP_DEFINES})
target_compile_options(qjsx-engine-target PRIVATE ${QJSX_ENGINE_OPTIONS} -O2 -ffunction-sections -fdata-sections -fno-math-errno)

# The interpreter loop keeps its bytecode pointer in a register (as on the
# RP2350, upstream QuickJS compiles the same way).
set_source_files_properties("${QJSX_ROOT}/engine/qjsx/core/interpreter.c" PROPERTIES COMPILE_OPTIONS "-fira-region=one")

# The hot functions of the engine run from the ITCM (as on the RP2350): the
# code in the flash goes through a 32 KB instruction cache. Each function has
# its own section: renamed .ramfunc.*, imxrt1062.ld places it in the ITCM,
# the start-up copies it there. The engine's sources are not touched.
set(QJSX_ITCM_FUNCTIONS
    # the interpreter loop (25 KB)
    JS_CallInternal
    # calls of native functions, closures
    js_call_c_function js_closure free_var_ref js_create_from_ctor
    # properties and objects
    JS_GetPropertyInternal JS_SetPropertyInternal add_property free_property
    JS_NewObject JS_NewObjectProtoClass JS_NewObjectFromShape js_new_shape2 js_free_shape0
    js_shape_hash_unlink free_gc_object
    # values, numbers, strings
    __JS_FreeValue __JS_FreeValueRT __JS_ToFloat64Free JS_ConcatString js_alloc_string
    js_alloc_string_rt js_new_string8_len
    # the engine's allocation functions, the garbage collector's trigger
    js_malloc js_malloc_rt js_realloc js_realloc_rt js_realloc_array js_free js_free_rt
    js_trigger_gc gc_decref
    # EXPERIMENT 2026-11-17 (await on the Teensy): the path of an await
    __js_malloc __js_free js_malloc_large async_func_init async_func_resume
    js_async_function_resume js_async_function_call async_func_free_frame __async_func_free
    async_func_free js_async_function_settle js_async_function_await_job
    js_async_function_await_enqueue JS_ExecutePendingJob close_var_refs)
set(QJSX_RENAMES "")
foreach(function ${QJSX_ITCM_FUNCTIONS})
    list(APPEND QJSX_RENAMES "--rename-section" ".text.${function}=.ramfunc.${function}")
endforeach()
add_custom_command(TARGET qjsx-engine-target POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} ${QJSX_RENAMES} $<TARGET_FILE:qjsx-engine-target>
    COMMENT "qjsx: engine functions moved to the ITCM: ${QJSX_ITCM_FUNCTIONS}")

# ### The firmware ############################################################

add_executable(qjsx.elf
    ${QJSX_COMMON_SOURCES}
    ${QJSX_FREERTOS_SOURCES}
    ${QJSX_FREERTOS_GPIO_SOURCES}
    ${QJSX_DEVICE_GPIO_SOURCES}
    ${QJSX_DEVICE_BOARD_SOURCES}
    ${QJSX_DEVICE_PWM_SOURCES}
    ${QJSX_NEWLIB_COMPAT_SOURCES}
    "${QJSX_ROOT}/ports/freertos/slot_storage.cpp"
    ${SDK_SOURCES}
    ${FREERTOS_SOURCES}
    "${QJSX_BOARD_DIR}/flash_config.c"
    "${MAIN_DIR}/boot_header.c"
    "${MAIN_DIR}/clock_imxrt1062.c"
    "${MAIN_DIR}/main.cpp"
    "${MAIN_DIR}/board_imxrt1062.cpp"
    "${MAIN_DIR}/platform_imxrt1062.cpp"
    "${MAIN_DIR}/flash_imxrt1062.cpp"
    "${MAIN_DIR}/usb_serial_imxrt1062.cpp"
    "${MAIN_DIR}/loader_imxrt1062.cpp"
    "${MAIN_DIR}/syscalls_imxrt1062.cpp"
    "${MAIN_DIR}/tls_imxrt1062.cpp"
    "${MAIN_DIR}/tls_read_tp_imxrt1062.S"
    "${MAIN_DIR}/atomics64_imxrt1062.c"
    "${MAIN_DIR}/hal_gpio_imxrt1062.cpp"
    "${MAIN_DIR}/hal_pwm_imxrt1062.cpp"
    "${MAIN_DIR}/native_profiler_imxrt1062.cpp")

# FreeRTOS's headers first (its list.h, not QuickJS's: our code never
# includes the engine's internal headers), then ours: main/ holds
# usb_device_config.h and fsl_os_abstraction.h for the SDK's USB stack
target_include_directories(qjsx.elf BEFORE PRIVATE "${FREERTOS_KERNEL_PATH}/include" "${FREERTOS_PORT_DIR}")
target_include_directories(qjsx.elf PRIVATE
    "${MAIN_DIR}"
    "${QJSX_BOARD_DIR}"
    "${QJSX_NEWLIB_COMPAT_DIR}"
    ${QJSX_INCLUDE_DIRS}
    ${SDK_INCLUDE_DIRS})
target_compile_definitions(qjsx.elf PRIVATE ${QJSX_DEFINES} ${SDK_DEFINES})
target_compile_options(qjsx.elf PRIVATE
    -O2 -ffunction-sections -fdata-sections -fno-math-errno
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics>)

# the SDK and the engine need the chip's definitions too
target_compile_definitions(qjsx-engine-target PRIVATE ${SDK_DEFINES})

# link: our script, unused sections dropped, a map file
target_link_libraries(qjsx.elf qjsx-engine-target m)
target_link_options(qjsx.elf PRIVATE
    "-T${MAIN_DIR}/imxrt1062.ld"
    "-Wl,--gc-sections"
    "-Wl,-Map=${CMAKE_BINARY_DIR}/qjsx.map"
    "-Wl,--print-memory-usage")
set_target_properties(qjsx.elf PROPERTIES LINK_DEPENDS "${MAIN_DIR}/imxrt1062.ld")

# the Intel hex file for teensy_loader_cli, and the sizes
add_custom_command(TARGET qjsx.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:qjsx.elf> "${CMAKE_BINARY_DIR}/qjsx.hex"
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:qjsx.elf>
    COMMENT "qjsx: qjsx.hex for the Teensy loader")
