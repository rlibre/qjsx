# What every device port builds (ESP32 with ESP-IDF, RP2350 with the pico-sdk,
# both CMake): the engine without its compiler (no eval on a device), the
# runtime, the agent and the debugger stub. The port adds its board
# (board.h), its platform (platform.h) and its entry point.
#
# Sets:
#   ESPJS_ROOT              root of the repository
#   ESPJS_QUICKJS_VERSION   engine/qjsx/VERSION
#   ESPJS_ENGINE_SOURCES    the engine (C)
#   ESPJS_COMMON_SOURCES    runtime, modules, agent (C++)
#   ESPJS_FREERTOS_SOURCES  the FreeRTOS part of platform.h, shared by the ports
#   ESPJS_INCLUDE_DIRS      every source folder (headers are included by name)
#   ESPJS_DEFINES           for every source
#   ESPJS_ENGINE_OPTIONS    compile options of the engine sources

get_filename_component(ESPJS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

file(READ "${ESPJS_ROOT}/engine/qjsx/VERSION" ESPJS_QUICKJS_VERSION)
string(STRIP "${ESPJS_QUICKJS_VERSION}" ESPJS_QUICKJS_VERSION)

# the engine, as the premake project qjsx-target: no compiler/
file(GLOB ESPJS_ENGINE_SOURCES
    "${ESPJS_ROOT}/engine/qjsx/core/*.c"
    "${ESPJS_ROOT}/engine/qjsx/bytecode/*.c"
    "${ESPJS_ROOT}/engine/qjsx/builtins/*.c")
list(APPEND ESPJS_ENGINE_SOURCES
    "${ESPJS_ROOT}/engine/qjsx/cutils.c"
    "${ESPJS_ROOT}/engine/qjsx/dtoa.c"
    "${ESPJS_ROOT}/engine/qjsx/libregexp.c"
    "${ESPJS_ROOT}/engine/qjsx/libunicode.c")

# the portable C++ code (common/), as on the desktop
file(GLOB ESPJS_COMMON_SOURCES
    "${ESPJS_ROOT}/common/runtime/*.cpp"
    "${ESPJS_ROOT}/common/runtime/modules/*.cpp"
    "${ESPJS_ROOT}/common/agent/*.cpp")

set(ESPJS_FREERTOS_SOURCES "${ESPJS_ROOT}/ports/freertos/platform_freertos.cpp")

set(ESPJS_INCLUDE_DIRS
    "${ESPJS_ROOT}/engine/qjsx"
    "${ESPJS_ROOT}/common/jsbind"
    "${ESPJS_ROOT}/common/runtime"
    "${ESPJS_ROOT}/common/runtime/modules"
    "${ESPJS_ROOT}/common/agent"
    "${ESPJS_ROOT}/common/board"
    "${ESPJS_ROOT}/ports/freertos")

set(ESPJS_DEFINES
    "CONFIG_VERSION=\"${ESPJS_QUICKJS_VERSION}\""
    "QJSX_NO_COMPILER"
    "ESPJS_NO_COMPILER")

# as in premake5.lua (qjsx-target); the engine is upstream code: its warnings
# are not ours to fix. On Xtensa and ARM int32_t is a long (32 bits, like
# int): QuickJS mixes int * and int32_t *, and %x with uint32_t, harmlessly,
# but GCC 14+ makes the first an error.
set(ESPJS_ENGINE_OPTIONS
    "-std=gnu11"
    "-Wno-incompatible-pointer-types"
    "-Wno-format"
    "-D_GNU_SOURCE"
    "-fwrapv"
    "-Wno-array-bounds"
    "-Wno-format-truncation"
    "-Wno-infinite-recursion"
    "-Wno-error")
