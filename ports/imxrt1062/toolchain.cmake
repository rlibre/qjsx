#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# CMake toolchain of the i.MX RT1062 firmware: the Arm GNU Toolchain
# (arm-none-eabi, its newlib), for a Cortex-M7 with its double precision FPU.
# ARM_TOOLCHAIN_PATH is given by tools/teensy.ps1 (the toolchain installed for
# the RP2350 in C:\tools\pico).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT ARM_TOOLCHAIN_PATH)
    message(FATAL_ERROR "qjsx: ARM_TOOLCHAIN_PATH is required (see tools/teensy.ps1)")
endif()

# CMake's compiler checks read this file again in their own projects: give
# them the path too
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ARM_TOOLCHAIN_PATH)

set(ARM_BIN "${ARM_TOOLCHAIN_PATH}/bin/arm-none-eabi-")
set(CMAKE_C_COMPILER "${ARM_BIN}gcc.exe")
set(CMAKE_CXX_COMPILER "${ARM_BIN}g++.exe")
set(CMAKE_ASM_COMPILER "${ARM_BIN}gcc.exe")
set(CMAKE_OBJCOPY "${ARM_BIN}objcopy.exe")
set(CMAKE_SIZE "${ARM_BIN}size.exe")

# no executable can run on the PC: compiler checks build a library
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# the core: Cortex-M7, Thumb, hardware double precision floating point
set(QJSX_CPU_FLAGS "-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT "${QJSX_CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${QJSX_CPU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${QJSX_CPU_FLAGS} -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${QJSX_CPU_FLAGS}")
