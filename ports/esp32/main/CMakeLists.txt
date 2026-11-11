# The firmware as one component: the sources shared by every device port
# (ports/cmake/espjs.cmake), the ESP32 board, platform and entry point, and the
# description of the chosen board (boards/<ESPJS_BOARD>/board_config.h).
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/espjs.cmake")
set(ESPJS_BOARD_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../boards/${ESPJS_BOARD}")

idf_component_register(
    SRCS
        ${ESPJS_ENGINE_SOURCES}
        ${ESPJS_COMMON_SOURCES}
        ${ESPJS_FREERTOS_SOURCES}
        "main.cpp"
        "board_esp32.cpp"
        "platform_esp32.cpp"
    INCLUDE_DIRS
        "."
        "${ESPJS_BOARD_INCLUDE}"
        ${ESPJS_INCLUDE_DIRS}
    REQUIRES
        esp_driver_uart
        esp_partition
        esp_psram
        esp_timer
        spi_flash
        vfs)

target_compile_definitions(${COMPONENT_LIB} PRIVATE ${ESPJS_DEFINES})
set_source_files_properties(${ESPJS_ENGINE_SOURCES} PROPERTIES COMPILE_OPTIONS "${ESPJS_ENGINE_OPTIONS}")
