# What every device port builds (ESP32 with ESP-IDF, RP2350 with the pico-sdk,
# both CMake): the engine without its compiler (no eval on a device), the
# runtime, the agent and the debugger stub. The port adds its board
# (board.h), its platform (platform.h) and its entry point.
#
# Sets:
#   QJSX_ROOT              root of the repository
#   QJSX_QUICKJS_VERSION   engine/qjsx/VERSION
#   QJSX_ENGINE_SOURCES    the engine (C)
#   QJSX_COMMON_SOURCES    runtime, modules, agent (C++)
#   QJSX_FREERTOS_SOURCES  the FreeRTOS part of platform.h, shared by the ports
#   QJSX_INCLUDE_DIRS      every source folder (headers are included by name)
#   QJSX_DEFINES           for every source
#   QJSX_ENGINE_OPTIONS    compile options of the engine sources

get_filename_component(QJSX_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

file(READ "${QJSX_ROOT}/engine/qjsx/VERSION" QJSX_QUICKJS_VERSION)
string(STRIP "${QJSX_QUICKJS_VERSION}" QJSX_QUICKJS_VERSION)

# the engine, as the premake project qjsx-engine-target: no compiler/
file(GLOB QJSX_ENGINE_SOURCES
    "${QJSX_ROOT}/engine/qjsx/core/*.c"
    "${QJSX_ROOT}/engine/qjsx/bytecode/*.c"
    "${QJSX_ROOT}/engine/qjsx/builtins/*.c")
list(APPEND QJSX_ENGINE_SOURCES
    "${QJSX_ROOT}/engine/qjsx/cutils.c"
    "${QJSX_ROOT}/engine/qjsx/dtoa.c"
    "${QJSX_ROOT}/engine/qjsx/libregexp.c"
    "${QJSX_ROOT}/engine/qjsx/libunicode.c")

# the portable C++ code (common/), as on the desktop
file(GLOB QJSX_COMMON_SOURCES
    "${QJSX_ROOT}/common/runtime/*.cpp"
    "${QJSX_ROOT}/common/runtime/modules/*.cpp"
    "${QJSX_ROOT}/common/agent/*.cpp")

set(QJSX_FREERTOS_SOURCES "${QJSX_ROOT}/ports/freertos/platform_freertos.cpp")

set(QJSX_INCLUDE_DIRS
    "${QJSX_ROOT}/engine/qjsx"
    "${QJSX_ROOT}/common/jsbind"
    "${QJSX_ROOT}/common/runtime"
    "${QJSX_ROOT}/common/runtime/modules"
    "${QJSX_ROOT}/common/agent"
    "${QJSX_ROOT}/common/board"
    "${QJSX_ROOT}/ports/freertos")

set(QJSX_DEFINES
    "CONFIG_VERSION=\"${QJSX_QUICKJS_VERSION}\""
    "QJSX_NO_COMPILER"
    "QJSX_NO_COMPILER")

# as in premake5.lua (qjsx-engine-target); the engine is upstream code: its warnings
# are not ours to fix. On Xtensa and ARM int32_t is a long (32 bits, like
# int): QuickJS mixes int * and int32_t *, and %x with uint32_t, harmlessly,
# but GCC 14+ makes the first an error.
set(QJSX_ENGINE_OPTIONS
    "-std=gnu11"
    "-Wno-incompatible-pointer-types"
    "-Wno-format"
    "-D_GNU_SOURCE"
    "-fwrapv"
    "-Wno-array-bounds"
    "-Wno-format-truncation"
    "-Wno-infinite-recursion"
    "-Wno-error")
