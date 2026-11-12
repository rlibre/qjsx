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

# device/usb on the chips with USB OTG, through esp_tinyusb (idf_component.yml
# fetches it for the same chips); main.cpp registers it when QJSX_HAS_USB is set
set(QJSX_USB_TARGETS esp32s2 esp32s3 esp32p4)
set(QJSX_USB_SOURCES "")
set(QJSX_USB_REQUIRES "")
set(QJSX_USB_DEFINES "")
if(IDF_TARGET IN_LIST QJSX_USB_TARGETS)
    set(QJSX_USB_SOURCES ${QJSX_DEVICE_USB_SOURCES} "hal_usb_esp32.cpp")
    set(QJSX_USB_REQUIRES espressif__esp_tinyusb)
    set(QJSX_USB_DEFINES QJSX_HAS_USB=1)
endif()

idf_component_register(
    SRCS
        ${QJSX_ENGINE_SOURCES}
        ${QJSX_COMMON_SOURCES}
        ${QJSX_FREERTOS_SOURCES}
        ${QJSX_FREERTOS_GPIO_SOURCES}
        ${QJSX_DEVICE_GPIO_SOURCES}
        ${QJSX_DEVICE_RMT_SOURCES}
        ${QJSX_DEVICE_I2C_SOURCES}
        ${QJSX_DEVICE_ADC_SOURCES}
        ${QJSX_DEVICE_SERIAL_SOURCES}
        ${QJSX_USB_SOURCES}
        "main.cpp"
        "board_esp32.cpp"
        "platform_esp32.cpp"
        "hal_gpio_esp32.cpp"
        "hal_rmt_esp32.cpp"
        "hal_i2c_esp32.cpp"
        "hal_adc_esp32.cpp"
        "hal_serial_esp32.cpp"
    INCLUDE_DIRS
        "."
        "${QJSX_BOARD_INCLUDE}"
        ${QJSX_INCLUDE_DIRS}
    REQUIRES
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_rmt
        esp_driver_uart
        esp_partition
        esp_psram
        esp_timer
        spi_flash
        vfs
        ${QJSX_USB_REQUIRES})

target_compile_definitions(${COMPONENT_LIB} PRIVATE ${QJSX_DEFINES} ${QJSX_USB_DEFINES})
set_source_files_properties(${QJSX_ENGINE_SOURCES} PROPERTIES COMPILE_OPTIONS "${QJSX_ENGINE_OPTIONS}")
