#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# What the firmware of the Waveshare ESP32-P4-Module-DEV-KIT with its
# 7-DSI-TOUCH-A adds: sys/draw, and its display (MIPI-DSI, ILI9881C, powered
# through an I2C circuit of the display): the board's code and the driver it
# uses (main/CMakeLists.txt adds them).
set(QJSX_BOARD_DRAW ON)
set(QJSX_BOARD_SCREEN ON)
set(QJSX_BOARD_SOURCES
    "${QJSX_BOARD_INCLUDE}/board_screen.cpp"
    "${QJSX_ROOT}/common/drivers/ili9881c.cpp")
