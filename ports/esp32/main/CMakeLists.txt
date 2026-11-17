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

# device/dac on the chips with a DAC (SOC_DAC_SUPPORTED: the original ESP32
# and the S2), through esp_driver_dac (dac_oneshot); main.cpp registers it
# when QJSX_HAS_DAC is set. The DAC's full scale comes from the board
# (DAC_FULL_SCALE_MV of board_config.h)
set(QJSX_DAC_TARGETS esp32 esp32s2)
set(QJSX_DAC_SOURCES "")
set(QJSX_DAC_REQUIRES "")
set(QJSX_DAC_DEFINES "")
if(IDF_TARGET IN_LIST QJSX_DAC_TARGETS)
    set(QJSX_DAC_SOURCES ${QJSX_DEVICE_DAC_SOURCES} "hal_dac_esp32.cpp")
    set(QJSX_DAC_REQUIRES esp_driver_dac)
    set(QJSX_DAC_DEFINES QJSX_HAS_DAC=1)
endif()

# what the board adds (boards/<board>/board.cmake, optional): QJSX_BOARD_DRAW
# when it has a display
set(QJSX_BOARD_DRAW OFF)
include("${QJSX_BOARD_INCLUDE}/board.cmake" OPTIONAL)

# sys/draw (drawing on a display, common/draw, for stdlib/display): on the
# boards whose board.cmake asks for it; on another one for a try, with
# idf.py -DQJSX_DRAW=ON (stays in the CMake cache of the build folder).
# main.cpp registers it when QJSX_HAS_DRAW is set
set(QJSX_DRAW_BUILD_SOURCES "")
set(QJSX_DRAW_DEFINES "")
if(QJSX_BOARD_DRAW OR QJSX_DRAW)
    set(QJSX_DRAW_BUILD_SOURCES ${QJSX_DRAW_SOURCES})
    set(QJSX_DRAW_DEFINES QJSX_HAS_DRAW=1)
endif()

idf_component_register(
    SRCS
        ${QJSX_ENGINE_SOURCES}
        ${QJSX_COMMON_SOURCES}
        ${QJSX_FREERTOS_SOURCES}
        ${QJSX_FREERTOS_GPIO_SOURCES}
        ${QJSX_DEVICE_BOARD_SOURCES}
        ${QJSX_DEVICE_GPIO_SOURCES}
        ${QJSX_DEVICE_RMT_SOURCES}
        ${QJSX_DEVICE_I2C_SOURCES}
        ${QJSX_DEVICE_SPI_SOURCES}
        ${QJSX_DEVICE_ADC_SOURCES}
        ${QJSX_DEVICE_PWM_SOURCES}
        ${QJSX_DEVICE_SERIAL_SOURCES}
        ${QJSX_USB_SOURCES}
        ${QJSX_DAC_SOURCES}
        ${QJSX_DRAW_BUILD_SOURCES}
        "main.cpp"
        "board_esp32.cpp"
        "platform_esp32.cpp"
        "hal_gpio_esp32.cpp"
        "hal_rmt_esp32.cpp"
        "hal_i2c_esp32.cpp"
        "hal_spi_esp32.cpp"
        "hal_adc_esp32.cpp"
        "hal_pwm_esp32.cpp"
        "hal_serial_esp32.cpp"
    INCLUDE_DIRS
        "."
        "${QJSX_BOARD_INCLUDE}"
        ${QJSX_INCLUDE_DIRS}
    REQUIRES
        bootloader_support
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_rmt
        esp_driver_spi
        esp_driver_uart
        esp_hw_support
        esp_partition
        esp_psram
        esp_timer
        joltwallet__littlefs
        mbedtls
        spi_flash
        vfs
        ${QJSX_USB_REQUIRES}
        ${QJSX_DAC_REQUIRES})

target_compile_definitions(${COMPONENT_LIB} PRIVATE ${QJSX_DEFINES} ${QJSX_USB_DEFINES} ${QJSX_DAC_DEFINES} ${QJSX_DRAW_DEFINES})

# a board without PSRAM: its JS heap is a region of internal RAM
# (board_esp32.cpp), the engine's arenas are sized for it. To compare,
# idf.py -DQJSX_JS_HEAP_SHARED=ON gives the heap shared with the firmware and
# QuickJS's own arenas back (stays in the CMake cache until set OFF)
if(QJSX_JS_HEAP_SHARED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE QJSX_JS_HEAP_SHARED)
elseif(NOT CONFIG_SPIRAM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${QJSX_SMALL_HEAP_DEFINES})
endif()
set_source_files_properties(${QJSX_ENGINE_SOURCES} PROPERTIES COMPILE_OPTIONS "${QJSX_ENGINE_OPTIONS}")
