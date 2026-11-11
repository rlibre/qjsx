# espjs firmware for the ESP32 boards (ESP-IDF project). One firmware per board:
# the board is chosen with ESPJS_BOARD (a folder of boards/), each board in its
# own build folder, with its own sdkconfig. In the ESP-IDF PowerShell, from
# ports/esp32 (tools/idf.ps1 does it):
#   idf.py -B build-yd-esp32-s3-n16r8 -D ESPJS_BOARD=yd-esp32-s3-n16r8 build
#   idf.py -B build-yd-esp32-s3-n16r8 -D ESPJS_BOARD=yd-esp32-s3-n16r8 -p COM11 flash
# The sources shared by every device port are listed in ports/cmake/espjs.cmake;
# main/ is the ESP32 code shared by every ESP32 board; boards/<board>/ holds what
# differs (sdkconfig.board: chip, flash, partitions, PSRAM; board_config.h).
cmake_minimum_required(VERSION 3.16)

# the board
set(ESPJS_BOARD "yd-esp32-s3-n16r8" CACHE STRING "board folder in ports/esp32/boards")
set(ESPJS_BOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/boards/${ESPJS_BOARD}")
if(NOT EXISTS "${ESPJS_BOARD_DIR}/sdkconfig.board")
    message(FATAL_ERROR "espjs: unknown board '${ESPJS_BOARD}' (no boards/${ESPJS_BOARD}/sdkconfig.board)")
endif()

# its configuration: the common defaults, then the board's (which wins); the
# sdkconfig of each build folder, so that boards do not overwrite each other
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${ESPJS_BOARD_DIR}/sdkconfig.board")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espjs)
