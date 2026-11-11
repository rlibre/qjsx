#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# The firmware as one component: the sources shared by every device port
# (ports/cmake/qjsx.cmake), the ESP32 board, platform and entry point, and the
# description of the chosen board (boards/<QJSX_BOARD>/board_config.h).
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/qjsx.cmake")
set(QJSX_BOARD_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../boards/${QJSX_BOARD}")

idf_component_register(
    SRCS
        ${QJSX_ENGINE_SOURCES}
        ${QJSX_COMMON_SOURCES}
        ${QJSX_FREERTOS_SOURCES}
        ${QJSX_DEVICE_GPIO_SOURCES}
        ${QJSX_DEVICE_RMT_SOURCES}
        "main.cpp"
        "board_esp32.cpp"
        "platform_esp32.cpp"
        "hal_gpio_esp32.cpp"
        "hal_rmt_esp32.cpp"
    INCLUDE_DIRS
        "."
        "${QJSX_BOARD_INCLUDE}"
        ${QJSX_INCLUDE_DIRS}
    REQUIRES
        esp_driver_gpio
        esp_driver_rmt
        esp_driver_uart
        esp_partition
        esp_psram
        esp_timer
        spi_flash
        vfs)

target_compile_definitions(${COMPONENT_LIB} PRIVATE ${QJSX_DEFINES})
set_source_files_properties(${QJSX_ENGINE_SOURCES} PROPERTIES COMPILE_OPTIONS "${QJSX_ENGINE_OPTIONS}")
