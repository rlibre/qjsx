#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# qjsx firmware for the RP2350 boards (pico-sdk + FreeRTOS SMP, ARM cores). One
# firmware per board: the board is chosen with QJSX_BOARD (a folder of boards/),
# each board in its own build folder. tools/pico.ps1 runs CMake with the paths
# of the toolchain:
#   cmake -G Ninja -B build-olimex-pico2-xxl -D QJSX_BOARD=olimex-pico2-xxl
#         -D PICO_SDK_PATH=... -D FREERTOS_KERNEL_PATH=... -D PICO_TOOLCHAIN_PATH=...
# The sources shared by every device port are listed in ports/cmake/qjsx.cmake;
# main/ is the RP2350 code shared by every RP2350 board; boards/<board>/ holds
# what differs (board.cmake: the pico-sdk board; board_config.h).
cmake_minimum_required(VERSION 3.13)

# the board: its pico-sdk board and platform, before the SDK is loaded
set(QJSX_BOARD "olimex-pico2-xxl" CACHE STRING "board folder in ports/rp2350/boards")
set(QJSX_BOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/boards/${QJSX_BOARD}")
if(NOT EXISTS "${QJSX_BOARD_DIR}/board.cmake")
    message(FATAL_ERROR "qjsx: unknown board '${QJSX_BOARD}' (no boards/${QJSX_BOARD}/board.cmake)")
endif()
include("${QJSX_BOARD_DIR}/board.cmake")

# the pico-sdk and FreeRTOS (paths given by tools/pico.ps1)
if(NOT PICO_SDK_PATH OR NOT FREERTOS_KERNEL_PATH)
    message(FATAL_ERROR "qjsx: PICO_SDK_PATH and FREERTOS_KERNEL_PATH are required (see tools/pico.ps1)")
endif()
include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
include("${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake")

project(qjsx C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# the version of the firmware: the qjsx version, from the #define of
# common/runtime/version.h (shown by picotool info)
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../../common/runtime/version.h" QJSX_VERSION_LINE REGEX "#define QJSX_VERSION")
string(REGEX MATCH "\"([^\"]+)\"" QJSX_VERSION_QUOTED "${QJSX_VERSION_LINE}")
set(QJSX_VERSION "${CMAKE_MATCH_1}")

# the sources shared by every device port
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/qjsx.cmake")
set(QJSX_COMPAT_DIR "${CMAKE_CURRENT_LIST_DIR}/compat")

# the engine in its own library: its "list.h" (QuickJS's lists) would hide
# FreeRTOS's in the firmware's sources, and FreeRTOS's would hide it here.
# compat/ (pthread.h, dirent.h: what newlib lacks) comes before the system
# headers.
add_library(qjsx-engine-target STATIC ${QJSX_ENGINE_SOURCES})
target_include_directories(qjsx-engine-target PRIVATE "${QJSX_COMPAT_DIR}" "${QJSX_ROOT}/engine/qjsx")
target_compile_definitions(qjsx-engine-target PRIVATE ${QJSX_DEFINES})
target_compile_options(qjsx-engine-target PRIVATE ${QJSX_ENGINE_OPTIONS})

# what the pico-sdk gives the sources of the firmware (its libraries are not
# linked to this one): one section per function and per variable, so that the
# link drops what is not used and each function can be placed (below)
target_compile_options(qjsx-engine-target PRIVATE -ffunction-sections -fdata-sections -fno-math-errno)

# The interpreter loop: by default GCC keeps its bytecode pointer 'pc' in the
# stack frame of this huge function (loaded and stored again by every
# opcode); with one allocation region for the whole function, it stays in a
# register (measures of 2026-11-12, upstream QuickJS compiles the same way).
set_source_files_properties("${QJSX_ROOT}/engine/qjsx/core/interpreter.c" PROPERTIES COMPILE_OPTIONS "-fira-region=one")

# The hot functions of the engine run from SRAM: the code in flash goes through
# a 16 KB XIP cache (shared with the PSRAM), and the interpreter loop alone is
# larger. Each function has its own section (-ffunction-sections): renamed
# .time_critical.*, the pico-sdk's linker script copies it to SRAM at boot.
# The engine's sources are not touched.
set(QJSX_SRAM_FUNCTIONS
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
    js_trigger_gc gc_decref)
set(QJSX_RENAMES "")
foreach(function ${QJSX_SRAM_FUNCTIONS})
    list(APPEND QJSX_RENAMES "--rename-section" ".text.${function}=.time_critical.${function}")
endforeach()
add_custom_command(TARGET qjsx-engine-target POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} ${QJSX_RENAMES} $<TARGET_FILE:qjsx-engine-target>
    COMMENT "qjsx: engine functions moved to SRAM: ${QJSX_SRAM_FUNCTIONS}")

# the firmware: the common code and this port's
add_executable(qjsx
    ${QJSX_COMMON_SOURCES}
    ${QJSX_FREERTOS_SOURCES}
    ${QJSX_FREERTOS_GPIO_SOURCES}
    ${QJSX_DEVICE_GPIO_SOURCES}
    main/main.cpp
    main/hal_gpio_rp2350.cpp
    main/board_rp2350.cpp
    main/platform_rp2350.cpp
    main/psram_rp2350.cpp
    compat/posix_compat.cpp)

# FreeRTOS's headers first (its list.h, not QuickJS's: our code never includes
# the engine's internal headers), then ours
target_include_directories(qjsx BEFORE PRIVATE "${FREERTOS_KERNEL_PATH}/include")
target_include_directories(qjsx PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/main"
    "${QJSX_BOARD_DIR}"
    "${QJSX_COMPAT_DIR}"
    ${QJSX_INCLUDE_DIRS})

# the USB link (stdio_usb): connected as soon as the host enumerates the board,
# whatever DTR says; no "\n" -> "\r\n" translation (frames are binary, the
# host reads console text as the desktop agent writes it)
target_compile_definitions(qjsx PRIVATE
    ${QJSX_DEFINES}
    PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1
    PICO_STDIO_DEFAULT_CRLF=0
    PICO_USE_MALLOC_MUTEX=1
    "USBD_PRODUCT=\"qjsx\"")

# measures only: the JS heap in SRAM even when the board has PSRAM, to compare
# the speed of JS with and without it (-D QJSX_JS_HEAP_IN_SRAM=ON)
option(QJSX_JS_HEAP_IN_SRAM "JS heap in SRAM, PSRAM left unused (measures)" OFF)
if(QJSX_JS_HEAP_IN_SRAM)
    target_compile_definitions(qjsx PRIVATE QJSX_JS_HEAP_IN_SRAM=1)
endif()

target_link_libraries(qjsx
    qjsx-engine-target
    pico_stdlib
    pico_flash
    hardware_flash
    hardware_watchdog
    FreeRTOS-Kernel-Heap3)

# console and host link on the USB port only
pico_enable_stdio_usb(qjsx 1)
pico_enable_stdio_uart(qjsx 0)

pico_set_program_name(qjsx "qjsx")
pico_set_program_version(qjsx "${QJSX_VERSION}")
pico_add_extra_outputs(qjsx)
