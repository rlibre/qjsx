#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# qjsx firmware for the ESP32 boards (ESP-IDF project). One firmware per board:
# the board is chosen with QJSX_BOARD (a folder of boards/), each board in its
# own build folder, with its own sdkconfig. In the ESP-IDF PowerShell, from
# ports/esp32 (tools/idf.ps1 does it):
#   idf.py -B build-yd-esp32-s3-n16r8 -D QJSX_BOARD=yd-esp32-s3-n16r8 build
#   idf.py -B build-yd-esp32-s3-n16r8 -D QJSX_BOARD=yd-esp32-s3-n16r8 -p COM11 flash
# The sources shared by every device port are listed in ports/cmake/qjsx.cmake;
# main/ is the ESP32 code shared by every ESP32 board; boards/<board>/ holds what
# differs (sdkconfig.board: chip, flash, partitions, PSRAM; board_config.h).
cmake_minimum_required(VERSION 3.16)

# the board
set(QJSX_BOARD "yd-esp32-s3-n16r8" CACHE STRING "board folder in ports/esp32/boards")
set(QJSX_BOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/boards/${QJSX_BOARD}")
if(NOT EXISTS "${QJSX_BOARD_DIR}/sdkconfig.board")
    message(FATAL_ERROR "qjsx: unknown board '${QJSX_BOARD}' (no boards/${QJSX_BOARD}/sdkconfig.board)")
endif()

# its configuration: the common defaults, then the board's (which wins); the
# sdkconfig of each build folder, so that boards do not overwrite each other
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${QJSX_BOARD_DIR}/sdkconfig.board")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(qjsx)
