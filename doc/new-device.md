# Running qjsx on a new device

This guide is for a developer who wants to run qjsx on a piece of hardware it does not run on
yet. It assumes you know embedded C / C++ and the SDK of your chip, not this project. The
design behind it is in [architecture.md](architecture.md), section 5.5 ("Device ports"); this
guide is the procedure. All paths are relative to the root of the repository.

Vocabulary: a *device* is a board that runs the programs. The *host* is the development
machine, with the `qjsx` tool. A *package* (`.qjspkg`) is a compiled program. The *agent* is
the part of the firmware that talks with the tool (deploy, run, stop, debug).

## 1. Which case are you in?

What exists today:

| Target | Where | State |
|---|---|---|
| ESP32-S3, ESP-IDF 6.1 | `ports/esp32/` | works on one real board: VCC-GND YD-ESP32-S3, WROOM-1 N16R8 module (16 MB flash, 8 MB octal PSRAM), wired by its UART0 / COM port |
| Desktop "board" (`qjsx agent`) | `host/platform/board_host.cpp`, `host/platform/platform_host.cpp` | works; a stand-in for a board, and a second implementation of the same interfaces |
| RP2350 (Olimex PICO2-XXL, FreeRTOS + pico-sdk) | nothing: there is no `ports/rp2350/` | planned, **not written** |

Two cases:

1. **Your board has a chip family that is already ported (ESP32).** You add a board
   description: a folder with two small files, no C++ to write. Go to
   [section 3](#3-case-1-a-new-esp32-board).
2. **Your chip has no port.** You write a port: a build file, an implementation of two
   interfaces (`platform.h` and `board.h`) and an entry point. Read
   [section 2](#2-what-a-device-firmware-is-made-of), then
   [section 4](#4-case-2-a-new-chip-a-new-port). The RP2350 will be the first port written
   this way; until it exists, the ESP32 port and the desktop board are the only examples.

An ESP32 that is not an ESP32-S3 (ESP32, ESP32-C3, ESP32-C6...) is between the two cases: only
the `esp32s3` target has been built. See [3.7](#37-what-a-board-description-cannot-change-yet).

## 2. What a device firmware is made of

One firmware per board, generic: it is flashed once, then only packages are sent. The firmware
contains no JavaScript at all.

| Part | Source | Written per port? |
|---|---|---|
| QuickJS engine, without its compiler (no `eval` on a device) | `engine/qjsx/` except `compiler/`, built with `QJSX_NO_COMPILER` | no |
| Runtime: event loop, globals (`console`, timers, `process`), modules `fs` and `crypto`, package reader | `common/runtime/` | no |
| Device agent, debugger stub, runner of the installed package | `common/agent/` | no |
| System services: clock, fatal stop, threads, mutex, signal | implementation of `common/runtime/platform.h` | yes (shared between FreeRTOS ports: `ports/freertos/`) |
| The chip and its wiring: memory, package storage, links, console | implementation of `common/board/board.h` | yes |
| Entry point: creates the JS task, which calls `agent::runDevice()` | e.g. `ports/esp32/main/main.cpp` | yes |

Threads on a device:

- **The JS thread.** The task that calls `agent::runDevice()` (`common/agent/runner.cpp`). It
  runs the packages and never returns. All JavaScript runs on it.
- **Transport threads.** Each link to the host has its own reader thread, started by the
  board. It gives the received bytes to `agent->receive()`. Requests of the agent (upload,
  `RUN`, `STOP`, `RESET`, `INFO`) are handled on that thread, before `receive()` returns;
  debugger requests are queued for the JS thread.

Two rules follow from this design:

- **The firmware must be built from the same sources as the tool.** A package says which
  bytecode version it holds; the device refuses a package whose version differs from the one
  of its engine (`JS_GetBytecodeVersion()`), at upload (`Agent::handleUploadBegin`,
  `common/agent/agent.cpp`) and again at start (`bytecodeMatches`,
  `common/runtime/program.cpp`).
- **Everything a program imports travels in its package**, as bytecode. A device has no
  compiler.

## 3. Case 1: a new ESP32 board

### 3.1 What a board is

`ports/esp32/` is one ESP-IDF project for every ESP32 board:

| Path | Content |
|---|---|
| `ports/esp32/CMakeLists.txt` | picks the board, stacks the two configuration files |
| `ports/esp32/sdkconfig.defaults` | what does not depend on the board: custom partition table, console on UART0 at 921600 baud in "custom" mode, no secondary console, 240 MHz, `CONFIG_FREERTOS_HZ=1000`, no C++ exceptions, no RTTI |
| `ports/esp32/partitions/4MB.csv`, `8MB.csv`, `16MB.csv` | partition tables, by flash size, not by board |
| `ports/esp32/main/` | the code shared by every ESP32 board (`main.cpp`, `board_esp32.cpp`, `board_esp32.h`, `platform_esp32.cpp`) |
| `ports/esp32/boards/<board>/sdkconfig.board` | what differs: chip, flash size, partition table, PSRAM |
| `ports/esp32/boards/<board>/board_config.h` | data for the shared code: today only `BOARD_NAME` |

The only board today is `ports/esp32/boards/yd-esp32-s3-n16r8/`. Use it as the model.

### 3.2 Steps

1. **Find out what the board really has**: chip, flash size, PSRAM size, and whether the PSRAM
   is octal or quad. Ask the chip rather than the marking on the module (the author first read
   his module as N4R2; it is an N16R8): `esptool flash-id` reports the flash size and the PSRAM.
   It needs no firmware, see [3.5](#35-build-and-flash) for the command.
2. **Check the wiring to the host.** The only link today is UART0 through a USB-serial bridge
   (the "COM" port of the test board, a CH343). A board that only exposes the native USB of the
   chip cannot be used yet (see [3.7](#37-what-a-board-description-cannot-change-yet)).
3. **Create the folder** `ports/esp32/boards/<board>/`. The folder name is the board name you
   pass to the build.
4. **Write `sdkconfig.board`** ([3.3](#33-sdkconfigboard)).
5. **Write `board_config.h`** ([3.4](#34-board_configh)).
6. **Choose the partition table** that matches the flash size ([3.6](#36-partition-tables)).
7. **Build and flash** ([3.5](#35-build-and-flash)).
8. **Check the board** ([section 5](#5-checks-on-the-board)).

Nothing else changes: not `ports/esp32/CMakeLists.txt`, not `ports/esp32/main/`.

### 3.3 `sdkconfig.board`

The file of the test board, complete:

```
CONFIG_IDF_TARGET="esp32s3"

# flash and partitions
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/16MB.csv"

# PSRAM: octal, only through heap_caps_malloc(MALLOC_CAP_SPIRAM), for the JS heap
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_USE_CAPS_ALLOC=y
```

| Option | Meaning | Notes |
|---|---|---|
| `CONFIG_IDF_TARGET` | the chip | only `"esp32s3"` has been built |
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | flash size | ESP-IDF has one option per size (`..._4MB`, `..._8MB`...); only 16 MB has been tried |
| `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` | the table, a file of `ports/esp32/partitions/` | must match the flash size; `CONFIG_PARTITION_TABLE_CUSTOM=y` is already in `sdkconfig.defaults` |
| `CONFIG_SPIRAM` | the board has PSRAM | `board_esp32.cpp` tests it (`#if CONFIG_SPIRAM`) to put the JS heap in PSRAM |
| `CONFIG_SPIRAM_MODE_OCT` | octal PSRAM | depends on the module. The wrong mode fails at boot. The quad option of ESP-IDF is `CONFIG_SPIRAM_MODE_QUAD`: not tried |
| `CONFIG_SPIRAM_USE_CAPS_ALLOC` | PSRAM is reached only through `heap_caps_malloc(MALLOC_CAP_SPIRAM)` | keeps `malloc` (our C++ code, task stacks) in internal RAM; only the JS heap goes to PSRAM |

The file is read after `sdkconfig.defaults` (`SDKCONFIG_DEFAULTS` in
`ports/esp32/CMakeLists.txt`), so the board wins when both set an option.

A board **without PSRAM**: leave the three `CONFIG_SPIRAM...` lines out. `board::jsAllocator()`
then returns `nullptr` and QuickJS uses its default allocator, in internal RAM. This path is
written, **not tried**: how much JavaScript fits in the internal RAM of an ESP32 is not known,
and with that allocator the engine cannot ask the C library for the real size of a block
(`js_def_malloc_usable_size` returns 0, see [engine-changes.md](engine-changes.md), section I).

### 3.4 `board_config.h`

```cpp
#pragma once

namespace board {

const char *const BOARD_NAME = "YD-ESP32-S3 (WROOM-1 N16R8)";   // in the identity of the device

} // namespace board
```

Data only. `board_esp32.cpp` includes it by its bare name: the build adds the folder of the
chosen board to the include path (`QJSX_BOARD_INCLUDE` in `ports/esp32/main/CMakeLists.txt`).
`BOARD_NAME` is the only value read today. Pins and links are meant to go there later; they
are not there yet.

### 3.5 Build and flash

The board is chosen with `-D QJSX_BOARD=<folder of boards/>` (default `yd-esp32-s3-n16r8`).
Each board builds in its own folder, `ports/esp32/build-<board>/`, which also holds its
`sdkconfig`, so boards do not overwrite each other. An unknown board stops CMake with
`qjsx: unknown board '<board>'`.

From `ports/esp32`, in a shell where ESP-IDF 6.1 is loaded:

```
idf.py -B build-<board> -D QJSX_BOARD=<board> build
idf.py -B build-<board> -D QJSX_BOARD=<board> -p <port> flash
```

Always give `-B` and `-D` together. The firmware is `build-<board>/qjsx.bin`.

On the author's Windows machine `idf.py` only works inside the ESP-IDF PowerShell environment.
The wrapper `tools/idf.ps1` loads it, goes to `ports/esp32` and adds `-B` and `-D`. Its paths
are those of that machine (the ESP-IDF profile, the repository, a log in `C:\temp`): adapt them
before using it elsewhere.

| Command | Does |
|---|---|
| `tools/idf.ps1 board=<board> build` | builds the firmware of the board |
| `tools/idf.ps1 board=<board> -p COMx flash` | flashes it |
| `tools/idf.ps1 board=<board> reconfigure build` | runs CMake again, then builds |
| `tools/idf.ps1 board=<board> ninja -k 0` | runs ninja in the build folder, every error in one pass |
| `tools/idf.ps1 esptool -p COMx flash-id` | asks the chip for its flash size and PSRAM |

Without `board=<board>` the wrapper takes the default board.

Pitfalls:

- **After adding or removing a source file** in `common/` or `ports/`, use `reconfigure build`.
  The source lists are CMake globs (`ports/cmake/qjsx.cmake`); ninja keeps the old list.
- **After changing `sdkconfig.board` or `sdkconfig.defaults`**: ESP-IDF applies these defaults
  when it creates the `sdkconfig` of the build folder, and an existing
  `build-<board>/sdkconfig` keeps the values it already has. If a change has no effect, delete
  that file (or the build folder) and build again. This is ESP-IDF behaviour, not something
  this project controls.
- **ESP-IDF builds with `-Werror`.** A warning in our C++ code stops the build. The engine
  sources are compiled with their own options, which include `-Wno-error`
  ([4.3](#43-compiler-and-c-library-pitfalls)).

### 3.6 Partition tables

Tables go by flash size. Each one has a `packages` partition (type `data`, subtype `0x40`),
which `board_esp32.cpp` finds by that type, subtype and name, and cuts into two slots of half
its size.

| Table | Firmware | `packages` | Largest package | `data` (future `fs`) | State |
|---|---|---|---|---|---|
| `4MB.csv` | one `factory` slot of 2.5 MB, no OTA | 1 MB (2 x 512 KB) | 512 KB - 4 KB | 448 KB | written, not tried on a board |
| `8MB.csv` | `ota_0` + `ota_1`, 2.5 MB each | 1 MB (2 x 512 KB) | 512 KB - 4 KB | 1.875 MB | written, not tried on a board |
| `16MB.csv` | `ota_0` + `ota_1`, 3 MB each | 2 MB (2 x 1 MB) | 1 MB - 4 KB | 7.875 MB | used on the test board |

The firmware takes about 0.7 MB. The OTA slots are reserved; OTA itself is not written. The
`data` partition is reserved for a file system that is not written either.

For another flash size, add a table to `ports/esp32/partitions/` and name it in
`sdkconfig.board`. Keep a partition named `packages`, type `data`, subtype `0x40`: without it
the firmware prints `qjsx: no "packages" partition` and no package can be installed.

### 3.7 What a board description cannot change yet

These are in the shared code, the same for every ESP32 board:

- The host link is UART0 (`HOST_UART` in `ports/esp32/main/board_esp32.cpp`), at 921600 baud
  (`CONFIG_ESP_CONSOLE_UART_BAUDRATE` in `sdkconfig.defaults`). Native USB and WiFi links are
  not written.
- The JS task: 48 KB of stack, priority 5, pinned to core 1 (`JS_TASK_STACK`,
  `JS_TASK_PRIORITY`, `JS_TASK_CORE` in `ports/esp32/main/board_esp32.h`). A single-core chip
  has no core 1: not tried, this constant would have to move to the board.
- No pins: there is no hardware module (GPIO, I2C...) yet, so nothing reads a pin table.
- Other ESP32 families: not tried. Only `esp32s3` has been built and run.

## 4. Case 2: a new chip (a new port)

No port has been written by following this section yet: the ESP32 port came first and the
RP2350 port is planned. What follows describes what the code requires, with the ESP32 port and
the desktop board as examples.

### 4.1 What you write

1. A build for your SDK that compiles the common sources ([4.2](#42-the-common-build-file)).
2. `platform.h` for your system ([4.4](#44-platformh-function-by-function)). On FreeRTOS, most
   of it exists ([4.5](#45-what-freertos-ports-share)).
3. `board.h` for your chip ([4.6](#46-boardh-function-by-function)), which includes a link to
   the host ([4.8](#48-links-to-the-host)) and a package storage
   ([4.9](#49-package-storage)).
4. An entry point ([4.7](#47-entry-point)).

Suggested layout, after `ports/esp32/`: `ports/<chip>/` with the build files, the code shared
by every board of the chip, and one folder per board for what differs. Target-specific values
come from the board, not from `#if` in `common/`.

Before touching hardware, the same common code can be exercised on the PC: `qjsx agent` runs
`agent::runDevice()` with the desktop board, and `qjsx-target` is the tool linked with the
engine without compiler. `tests/run-tests.sh` uses both.

### 4.2 The common build file

`ports/cmake/qjsx.cmake` lists what every device port builds. It is plain CMake, meant to be
included by the ESP-IDF build and, later, by a pico-sdk build. It sets:

| Variable | Content |
|---|---|
| `QJSX_ROOT` | root of the repository |
| `QJSX_QUICKJS_VERSION` | text of `engine/qjsx/VERSION` |
| `QJSX_ENGINE_SOURCES` | the engine in C: `engine/qjsx/core/*.c`, `bytecode/*.c`, `builtins/*.c`, plus `cutils.c`, `dtoa.c`, `libregexp.c`, `libunicode.c`. No `compiler/` |
| `QJSX_COMMON_SOURCES` | our C++: `common/runtime/*.cpp`, `common/runtime/modules/*.cpp`, `common/agent/*.cpp` |
| `QJSX_FREERTOS_SOURCES` | `ports/freertos/platform_freertos.cpp`; add it only on a FreeRTOS port |
| `QJSX_INCLUDE_DIRS` | every source folder: headers are included by their bare name, so no two headers may share a name |
| `QJSX_DEFINES` | for every source: `CONFIG_VERSION="<QuickJS version>"` and `QJSX_NO_COMPILER` |
| `QJSX_ENGINE_OPTIONS` | compile options for the engine sources only |

How the ESP32 port uses it (`ports/esp32/main/CMakeLists.txt`): it includes the file, registers
one component with `QJSX_ENGINE_SOURCES`, `QJSX_COMMON_SOURCES`, `QJSX_FREERTOS_SOURCES` and
its own three files, adds `QJSX_INCLUDE_DIRS` and the folder of the chosen board to the include
path, applies `QJSX_DEFINES` to the whole component, and applies `QJSX_ENGINE_OPTIONS` to the
engine sources with `set_source_files_properties(... COMPILE_OPTIONS ...)`.

Do the same in your build. Our C++ code needs C++17 and is written without exceptions and
without RTTI; the ESP32 port states it in `sdkconfig.defaults`.

The source lists are globs: after adding or removing a file, run CMake again.

### 4.3 Compiler and C library pitfalls

- **`int32_t` is `long` on 32-bit GCC targets** (Xtensa, ARM), not `int`. QuickJS mixes
  `int *` and `int32_t *`, and `%x` with `uint32_t`. This is harmless, but GCC 14 and later
  make the first an error. Hence, in `QJSX_ENGINE_OPTIONS`: `-std=gnu11`,
  `-Wno-incompatible-pointer-types`, `-Wno-format`. The other options there are `-D_GNU_SOURCE`,
  `-fwrapv`, `-Wno-array-bounds`, `-Wno-format-truncation`, `-Wno-infinite-recursion` and
  `-Wno-error` (the engine is upstream code: its warnings are not ours to fix).
- **In our own code**, the same fact shows up as format warnings and as overloads that do not
  resolve the way they do on the PC. On 32 bits `JSValue` is a plain `uint64_t`: never
  specialize a template on it.
- **`-Werror`.** ESP-IDF builds with it. Expect the same from any SDK that does: our code must
  compile without warnings on your compiler.
- **C library.** Two small engine changes were needed for newlib, both keyed on `__NEWLIB__`
  (see [engine-changes.md](engine-changes.md), section I): `js_def_malloc_usable_size` returns
  0 (newlib does not declare `malloc_usable_size`), and `getTimezoneOffset` takes the path
  without `tm_gmtoff`. Note: the `sdkconfig` that ESP-IDF 6.1 generated for the test board
  selects picolibc with its newlib compatibility (`CONFIG_LIBC_PICOLIBC`,
  `CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY`), an ESP-IDF default that this project does not
  set. The firmware builds and runs that way. If your C library does not define `__NEWLIB__`
  and lacks the same two things, the engine needs the same two changes for it; engine changes
  are agreed with the author first.
- **`fseeko` / `ftello`** are used by `common/runtime/files.cpp` on everything that is not
  Windows. [known-bugs.md](known-bugs.md) lists them as still to be checked on a device.
- **Thread stacks are small on an RTOS** (a few KB). No array larger than about 256 bytes on a
  stack, in the port as in the common code. Read into a buffer allocated once on the heap, as
  `uartReader` does in `ports/esp32/main/board_esp32.cpp`.
- **Allocations.** Our code allocates through `memory::allocate` / `reallocate` / `release`
  (`common/runtime/memory.h`), which call `malloc` / `realloc` / `free`. Running out of memory
  is fatal (`platform::fatal`). The JS heap is separate ([4.10](#410-memory-budget)).

### 4.4 `platform.h`, function by function

`common/runtime/platform.h` is the system layer: what the event loop, the agent and the
debugger stub need from the operating system. Everything is in namespace `platform`.

| Function | What it must do | Called from | ESP32 port | Desktop |
|---|---|---|---|---|
| `int64_t monotonicMs()` | Milliseconds from an arbitrary origin, never going backwards. Use a 64-bit source: never the 32-bit FreeRTOS tick count, which wraps after 49 days at 1 kHz. Signed on purpose | the JS thread (timers of the loop); keep it safe from any thread | `esp_timer_get_time() / 1000` (`platform_esp32.cpp`) | `GetTickCount64`, `CLOCK_MONOTONIC` |
| `[[noreturn]] void fatal(const char *message)` | Print the message on the console, then stop for good. On a device: restart the chip. Must not return | any thread (out of memory in `memory::allocate`, a lock that cannot be created) | prints `qjsx: fatal: <message>`, then `esp_system_abort(message)`: the IDF panic handler prints the backtrace and restarts the chip | prints, then `abort()` |
| `void sleepMs(int64_t milliseconds)` | Block the calling thread. Never used on the JS thread while JS code waits (the loop waits on its mailbox instead) | producer threads. No caller in `common/` today | `vTaskDelay`, rounded up to ticks (`ports/freertos/`) | `Sleep`, `nanosleep` |
| `void yieldIfDue()` | From time to time, block the JS task for one tick so that lower-priority tasks of its core run; return at once the rest of the time. See below | the JS thread only, from the interrupt handler of the runtime (`Runner::interrupt`, `common/agent/runner.cpp`), so often while JS code runs | every 100 ms (`YIELD_PERIOD_MS`), `vTaskDelay(1)` (`ports/freertos/`) | empty |
| `bool startThread(ThreadFunction *run, void *argument, ThreadOptions options)` | Start a detached thread that runs `run(argument)` and ends when `run` returns. Return false on failure | the entry point (JS task), the board (reader threads) | `xTaskCreatePinnedToCore`; the task deletes itself when `run` returns (a FreeRTOS task must never return) | `CreateThread`, `pthread_create` + detach |
| `Mutex` | A lock, **recursive**: its owner may lock it again and unlocks as many times. The constructor creates it unlocked | any thread | FreeRTOS recursive mutex (`ports/freertos/`); creation failure is fatal | critical section, `PTHREAD_MUTEX_RECURSIVE` |
| `Signal` | A lock plus a wake-up: `lock()`, `unlock()`, `wait(timeoutMs)`, `notify()`. See below | producers call `notify()`; the JS thread waits | a mutex + a binary semaphore (`ports/freertos/`) | critical section or mutex + condition variable |
| `atomicIncrement`, `atomicDecrement` | Atomic reference counting | any thread | nothing to write: inline, GCC / Clang `__atomic_*` builtins | same |

**`ThreadOptions`.** `{ name, stackSize, priority, core }`, passed by value. `stackSize` is in
bytes; `DEFAULT_STACK` (0), `DEFAULT_PRIORITY` (0) and `ANY_CORE` (-1) leave the choice to the
platform, and `defaultThreadOptions(name)` sets all three. Higher priority numbers are more
urgent. On an RTOS every field counts. The ESP32 port maps the defaults to a 4096-byte stack,
priority 5 and no core affinity. Check the unit your RTOS expects for a stack size.

**Ownership in `startThread`.** On success the new thread owns `argument`. On failure the
caller still owns it. The ESP32 port passes a small heap record (`ThreadStart`) to the task,
which frees it; if the task cannot be created, `startThread` frees it.

**`yieldIfDue()`.** On an RTOS the JS task has a higher priority than the idle task. A program
that computes for a long time never returns to the event loop, so the idle task of its core
never runs. Under ESP-IDF the task watchdog then prints `task_wdt ... IDLE1` every 5 seconds,
and the idle task no longer frees deleted tasks. `yieldIfDue()` must block the JS task for one
tick about every 100 ms. With a 1 ms tick this costs at most 1 % of the computing time. The
function keeps its last pause in a variable without a lock: it is for the JS thread only.
Native code that spins without going through the interrupt handler of the runtime is not
covered: the watchdog message is then a real defect of the firmware.

**`Signal`.** Its users are the mailbox of the event loop (`common/runtime/loop.cpp`), the
runner (`common/agent/runner.cpp`) and the pause loop of the debugger stub
(`common/agent/debugger.cpp`). The contract:

- `wait(timeoutMs)` is called with the lock held; it releases the lock while it waits and
  holds it again when it returns. `timeoutMs < 0` waits forever. It returns false on timeout.
- It may return early: every caller checks its own condition again in a loop.
- `notify()` may be called with or without the lock. A `notify()` that comes between the
  release of the lock and the start of the wait must not be lost.
- One thread waits on a given `Signal`: the JS thread. The FreeRTOS implementation relies on
  it (one binary semaphore; several `notify()` before a wait count as one).
- Do not rely on `Signal::lock()` being recursive: it is not on FreeRTOS nor on POSIX.

**Locks created early.** `Mutex` objects exist as members of static objects (the `FlashStorage`
of `board_esp32.cpp` is one). Their constructors therefore run during static initialization,
before the entry point. Make sure your system allows creating a lock that early.

**Interrupt handlers.** The header mentions interrupts as event producers. With the present
code a producer must be a thread: `Mailbox::post` allocates memory and takes a lock, and the
FreeRTOS `Signal` uses the task-level semaphore calls. Posting from an interrupt handler is
not done.

### 4.5 What FreeRTOS ports share

`ports/freertos/platform_freertos.cpp` implements, with plain FreeRTOS calls: `sleepMs`,
`yieldIfDue`, `Mutex` and `Signal`. A FreeRTOS port adds `QJSX_FREERTOS_SOURCES` to its build
and writes only what differs between chips, as `ports/esp32/main/platform_esp32.cpp` does:

- `monotonicMs()`: a 64-bit clock of the SDK;
- `fatal()`: the SDK's way to print and restart;
- `startThread()`: task creation and core pinning.

The file already looks for the FreeRTOS headers in both layouts (`freertos/FreeRTOS.h` as in
ESP-IDF, `FreeRTOS.h` otherwise). It has only been compiled with ESP-IDF.

`yieldIfDue()` there assumes a tick short enough for "one tick every 100 ms" to be cheap; the
ESP32 port sets `CONFIG_FREERTOS_HZ=1000`.

A port on another system implements all of `platform.h`. `host/platform/platform_host.cpp`
(Win32 and POSIX) is the complete example.

### 4.6 `board.h`, function by function

`common/board/board.h` is the chip and its wiring. Everything is in namespace `board`. The
ESP32 implementation is `ports/esp32/main/board_esp32.cpp`; the desktop one is
`host/platform/board_host.cpp`.

| Function | What it must do | Called from | Ownership | ESP32 port | Desktop |
|---|---|---|---|---|---|
| `void identity(Identity *identity)` | Fill `chip`, `board`, `flashSize` and `psramSize` (bytes, 0 if unknown or none) | **no caller in `common/` today**: both boards implement it, the agent does not send it yet | the strings are static | `CONFIG_IDF_TARGET`, `BOARD_NAME`, `esp_flash_get_size`, `esp_psram_get_size` | `"desktop"`, `"qjsx agent"`, 0, 0 |
| `const char *platformName()` | The value of `process.platform` | the JS thread, when the globals of a run are installed | a static string | `"esp32"` | `"win32"`, `"darwin"`, `"linux"` |
| `const char *dataDirectory()` | Root of the program's files: `process.cwd()`, and what `fs` resolves relative paths against. Absolute, `/` separators. `nullptr` if the board has no file system | the JS thread, once, at the start of `agent::runDevice()` | the runtime keeps the pointer: static, or owned by the board for good | `nullptr`: `process.cwd()` is `"/"`, `fs` fails with `ENOSYS` | absolute `<storage>/data` |
| `bool restart()` | Restart the chip; does not return on a device. Return false if the board cannot restart: the runner then runs the program again | a transport thread, on a `RESET` request, after the agent has sent its answer | | `esp_restart()` | returns false |
| `const JSMallocFunctions *jsAllocator()` | The allocator of the JS heap, or `nullptr` for QuickJS's default one (the C heap) | the JS thread, at the start of each run (`createRuntime`, `common/runtime/program.cpp`) | a static structure | PSRAM through `heap_caps_malloc` / `heap_caps_realloc` / `heap_caps_free` / `heap_caps_get_allocated_size`; `nullptr` without `CONFIG_SPIRAM` | `nullptr` |
| `size_t jsStackSize()` | Bytes of stack the JS code may use, given to `JS_SetMaxStackSize`. A bit less than the stack of the JS task. 0: QuickJS's default | the JS thread, at the start of each run | | `JS_STACK_LIMIT`, 40 KB of the 48 KB task stack | 0 |
| `agent::Storage *packageStorage()` | Where uploaded packages are written ([4.9](#49-package-storage)) | the JS thread: twice in `agent::runDevice()`, then at each run | the board owns the object; return the same one at every call, valid for good | a static `FlashStorage`, opened at the first call | a static `FileStorage` |
| `const uint8_t *openPackage(size_t *size)` | The installed package, readable in place until `closePackage()`. `nullptr` if none | the JS thread, at the start of each run | the board owns the bytes; one package open at a time | `esp_partition_mmap` of the installed slot | reads the file into memory |
| `void closePackage(const uint8_t *package)` | Release what `openPackage()` returned | the JS thread, when the run is over (`rt::runPackage` has returned) | | `esp_partition_munmap` | frees the copy |
| `bool startLinks(agent::Agent *agent)` | Start the links to the host ([4.8](#48-links-to-the-host)). Return false, after printing why, if none could start | the JS thread, once, in `agent::runDevice()`, before the installed package starts | the agent is borrowed; it lives as long as the device | installs the UART0 driver, attaches the link, starts the reader task | listens on TCP, starts the accept thread |
| `void writeConsole(int stream, const char *text, size_t length)` | Write the program's output when no host session is open. `stream` is `rt::OUTPUT_STDOUT` (1) or `rt::OUTPUT_STDERR` (2); `text` is not zero-terminated | the thread that produced the output: the JS thread | `text` is borrowed for the call | `fwrite` on `stdout` / `stderr`, then `fflush` (the IDF console, UART0) | the same on the PC |

Notes:

- **`identity` vs `agent::Identity`.** There are two structures. `board::Identity` (above)
  describes the hardware and is not transmitted today. `agent::Identity`
  (`common/agent/agent.h`) is what the entry point gives to `agent::runDevice()`, and its
  `name` and `engine` are what the host sees in the answer to `HELLO`
  ([4.7](#47-entry-point)).
- **`restart()` and the answer to `RESET`.** The agent hands its answer to `Link::send` before
  it calls `restart()`. Nothing waits for the bytes to leave the wire: the ESP32 port does not
  flush before `esp_restart()`.
- **The JS allocator.** Four functions, as in `board_esp32.cpp`: allocate, free (a null block
  does nothing), resize (size 0 frees), and the real size of a block, which QuickJS uses to
  count its memory. Running out of JS heap is not fatal: QuickJS turns it into a JS exception.
- **`jsStackSize()`.** QuickJS turns going past the limit into a `RangeError` instead of a
  crash. The limit is measured from the point where the runtime is created, on the JS thread,
  so leave room for the C code that runs under the JS code (8 KB on the ESP32).
- **A board without file system** returns `nullptr` from `dataDirectory()`. That is every
  device today.

### 4.7 Entry point

The entry point starts one task with a large stack; that task becomes the JS thread. From
`ports/esp32/main/main.cpp`:

```cpp
void jsTask(void *) {
    // the agent's identity in HELLO; no token on a serial link
    agent::Identity identity = { "qjsx " CONFIG_IDF_TARGET, CONFIG_VERSION, nullptr, JS_GetBytecodeVersion() };
    agent::runDevice(identity);

    // runDevice returns only if the links could not start
    printf("qjsx: the device could not start\n");
}

extern "C" void app_main(void) {
    platform::ThreadOptions options = { "js", board::JS_TASK_STACK, board::JS_TASK_PRIORITY, board::JS_TASK_CORE };
    if (!platform::startThread(&jsTask, nullptr, options)) {
        printf("qjsx: cannot start the JS task\n");
    }
}
```

`agent::Identity` (`common/agent/agent.h`):

| Field | Content | ESP32 |
|---|---|---|
| `name` | name of the agent, shown by the tool when it connects | `"qjsx esp32s3"` |
| `engine` | QuickJS version | `CONFIG_VERSION`, from `engine/qjsx/VERSION` |
| `token` | required in `HELLO` if not null; for network links | `nullptr` (serial link) |
| `bytecode` | version of the bytecode the engine reads | `JS_GetBytecodeVersion()` |

The strings must outlive the agent: use literals.

If the board needs preparing before the common code runs (clocks, PSRAM, flash, a storage
directory), do it before `agent::runDevice()`. ESP-IDF does it before `app_main`; the desktop
calls `board::configureHost()` first (`host/agent_command.cpp`).

What `agent::runDevice()` then does, in this order, on the calling thread:

1. Builds the runner, the agent (with `board::packageStorage()`) and the debugger stub. They
   live on the stack of the JS task, for good.
2. Routes the program's output: to the host as `LOG` events during a session, else to
   `board::writeConsole()`.
3. Calls `rt::setDataDirectory(board::dataDirectory())`.
4. Calls `board::startLinks()`. If it fails, `runDevice()` returns.
5. If a package is installed (`packageStorage()->installed(...)`), asks for a run: a device
   starts its program at boot.
6. Runs the packages the host asks for (`RUN`, `RESET`), forever.

Each run: `board::openPackage()`, a `STARTED` event, a new runtime (`board::jsAllocator()`,
`board::jsStackSize()`), the program, `board::closePackage()`, an `EXITED` event with the exit
code. At boot without an installed package the device just waits; a `RUN` request then prints
`qjsx agent: no package installed`.

You do not implement `agent::Controller` (`run`, `stop`, `reset`, `running`): the common
runner does.

### 4.8 Links to the host

A link is a byte stream in both directions. The protocol does its own framing (and checks each
frame), so the link needs no packet boundary of its own. The board gives the agent:

- **Outgoing bytes**: an `agent::Link`, with one method.

  ```cpp
  virtual bool send(const uint8_t *data, size_t size) = 0;
  ```

  It sends whole frames. The agent calls it with its own mutex held, from any thread (answers
  on the transport thread, events and program output on the JS thread), so two frames never
  interleave. Return false on a write error; the agent ignores it, and the transport is
  expected to notice a broken link by itself.
- **Incoming bytes**: a reader thread, started by `startLinks()`, that calls
  `agent->receive(buffer, size)` with whatever arrived, cut anywhere. Complete agent requests
  are handled inside `receive()`, on that thread: package uploads, so flash erases and writes,
  run there. The stack of the reader must be large enough for that; the ESP32 port gives it
  the default 4096 bytes. Read into a heap buffer, not a stack array.
- **Attachment**: `agent->attach(&link)` tells the agent where to send. A link that is always
  there (serial) is attached once, in `startLinks()`. A link with connections (TCP) calls
  `attach(link)` when a host connects and `attach(nullptr)` when it leaves, as
  `host/platform/board_host.cpp` does. `attach()` closes the session: the host must send
  `HELLO` again.

The agent has one current link (`Agent::attach` replaces it). The design
([architecture.md](architecture.md), 5.5) is that a board starts every link it can handle and
that the link receiving `HELLO` carries the session; with one link per board today, this has
not been needed and is not written.

**Sharing the wire with the console (ESP32, UART0).** UART0 carries both the frames and the
console text (`printf`, ESP-IDF logs). `startLinks()` installs the UART driver, then routes
`stdout` / `stderr` through it (`uart_vfs_dev_use_driver`). `UartLink::send` writes a frame
with one `uart_write_bytes` call. Every write goes through the same driver, one call at a time,
so a frame is never cut by text. The host takes the bytes outside frames as console text. If
your link shares a wire with a console, keep this property: one write call per frame, through
the same driver as the console.

During a session, the output of the JS program does not go to the console: it travels as `LOG`
events on the link. Text printed by C code still goes out as raw text between frames.

A known defect on a permanent link is listed in [known-bugs.md](known-bugs.md): the agent only
forgets its last answers in `attach()`, which a serial link calls once.

Not written on any device: native USB, TCP over WiFi. The desktop board shows a TCP link
(one host at a time, a new host replaces the previous one).

### 4.9 Package storage

The board implements `agent::Storage` (`common/agent/agent.h`) and the pair
`openPackage()` / `closePackage()`.

| Method | What it must do |
|---|---|
| `bool beginUpload(size_t size)` | Prepare room for a package of `size` bytes. Must not touch the installed package. Return false if it cannot hold it. An unfinished previous upload has been dropped with `abortUpload()` first |
| `bool writeUpload(size_t offset, const uint8_t *data, size_t size)` | Write a chunk. Chunks come in order, without gaps (the agent checks). The tool sends 2048 bytes at a time (`UPLOAD_CHUNK`, `host/js/protocol.js`). After an error the upload stays open and the host may send the chunk again |
| `bool finishUpload(const char *buildId)` | Make the uploaded package the installed one, with its build id. Called only when every byte has arrived and the SHA-256 matches (the agent checks both). Ends the upload, whether it succeeds or not |
| `void abortUpload()` | Drop the upload. The installed package stays |
| `bool installed(char *buildId, size_t buildIdSize, size_t *packageSize)` | Build id (zero-terminated) and size of the installed package. Return false if none |

Threads: the upload methods run on the transport thread, one upload at a time. `installed()`
is called on the transport thread (`INFO`) **and** on the JS thread (at boot and at each run),
and `openPackage()` / `closePackage()` on the JS thread. Protect what they share; the ESP32
port uses a `platform::Mutex`. A build id is at most 63 characters.

What the storage must guarantee:

- **An interrupted upload leaves the installed package as it was.** Power can go at any time.
- **The package of the running program is never erased under it.** The program reads it in
  place while it runs.
- **`finishUpload()` is the only step that changes what is installed**, and it is as close to
  atomic as the medium allows.

How the ESP32 port does it (`FlashStorage` in `ports/esp32/main/board_esp32.cpp`), a model for
any flash:

1. The `packages` partition is cut into **two slots**, each half of the partition.
2. A slot starts with a **header** in its first sector (4096 bytes): a magic number, a
   sequence number, the size of the package, the build id. The package follows.
3. The **installed** package is the slot with a valid header and the highest sequence. A
   header is valid when its magic is right, its size fits the slot and its build id is
   terminated.
4. `beginUpload()` takes the slot that is not installed. It refuses if the package does not
   fit (`size > slot size - 4096`) or if that slot is the one the running program is mapped
   from. It then erases the header sector and the room of the package, rounded up to a sector.
5. `writeUpload()` writes each chunk after the header.
6. `finishUpload()` writes the **header last**, with a sequence one above the installed one.
   Until then the slot has no valid header.
7. `openPackage()` maps the installed slot after its header (`esp_partition_mmap`);
   `closePackage()` unmaps it. One mapping at a time.

A consequence of step 4: after an install that is not followed by a run (`qjsx deploy
--no-run`), the old program still runs from its slot, and the next upload needs that same
slot. It is refused until the program is stopped or run again.

**Read in place.** `openPackage()` must return a pointer to the whole package, valid until
`closePackage()`. A package is a zip whose entries are stored without compression, so the
reader (`common/runtime/package.*`) uses it without copying it to RAM. On a chip whose flash
is memory-mapped, return the mapped address. The engine still copies the bytecode of each
module into the JS heap when it loads it (`JS_ReadObject` without `JS_READ_OBJ_ROM_DATA`):
running the bytecode itself from flash is not done.

The desktop board is the other example: an upload is kept in memory, then written to
`<storage>/app.qjspkg` with its build id in `app.buildid`, through temporary files and
renames; `openPackage()` reads the file into memory.

Not tried: a flash API that only writes whole pages, or that cannot write while code runs from
the same flash. The ESP32 API accepts the chunks as they come.

### 4.10 Memory budget

- **JS heap.** Where `board::jsAllocator()` puts it: the 8 MB PSRAM on the test board.
- **JS stack.** The stack of the JS task, in internal RAM: 48 KB on the ESP32, of which
  `board::jsStackSize()` lets JS code use 40 KB. The runner, the agent and the debugger stub
  also live on that stack.
- **C heap** (`malloc`): everything our C++ code allocates. The largest block is fixed: the
  frame parser of the agent allocates twice the maximum message size when the agent is built,
  2 x 65536 bytes (`DEFAULT_MAX_MESSAGE`, `common/agent/protocol.h`). On the ESP32 this is
  internal RAM.
- **Reader task**: 4096 bytes of stack, a 256-byte read buffer, and the 4096-byte receive and
  transmit buffers of the UART driver, on the ESP32.

A chip without PSRAM, with the JS heap in internal RAM: not tried.

### 4.11 Checklist for a new port

Build:

- [ ] The engine sources compile with `QJSX_ENGINE_OPTIONS`; `compiler/` is not built;
      `QJSX_NO_COMPILER` is defined for every source.
- [ ] Our C++ code compiles without warnings, without exceptions and without RTTI.
- [ ] The firmware is built from the same sources as the `qjsx` tool you will use.

`platform.h`:

- [ ] `monotonicMs()` uses a 64-bit clock.
- [ ] `fatal()` prints, restarts the chip and never returns.
- [ ] `startThread()` honours the stack size, priority and core; a thread whose function
      returns ends cleanly.
- [ ] `Mutex` is recursive.
- [ ] `Signal::wait()` loses no `notify()`, with and without a timeout.
- [ ] `yieldIfDue()` lets the idle task run: a program that loops for a minute triggers no
      watchdog message.

`board.h`:

- [ ] The JS task has a large stack and `jsStackSize()` is below it: unbounded recursion in JS
      ends with a `RangeError`, not a crash.
- [ ] The JS heap is where you want it (`jsAllocator()`).
- [ ] `startLinks()` prints why when it fails, and the reader reads into a heap buffer.
- [ ] A frame is never cut by console text on a shared wire.
- [ ] An upload cut in the middle (unplug the board) leaves the previous package installed.
- [ ] The installed package starts at boot.
- [ ] A package larger than the storage is refused, and the installed one stays.
- [ ] `restart()` restarts the chip.

Then the checks of [section 5](#5-checks-on-the-board).

## 5. Checks on the board

Run them in this order, from the root of the repository, with the `qjsx` tool built from the
same sources as the firmware (`bin/Release/qjsx`). The examples use the serial target of the
ESP32 port; replace `COMx` with the port of the board.

The baud rate matters: a `serial:` target without `@<baud rate>` opens the port at 115200,
and the ESP32 firmware talks at 921600.

1. **Hardware, before any firmware (ESP32).** `esptool flash-id` on the port of the board
   (`tools/idf.ps1 esptool -p COMx flash-id`) gives the flash size and the PSRAM. They must
   match `sdkconfig.board`.

2. **The firmware boots.** On the console the ESP32 firmware prints
   `qjsx agent: listening on UART0`. A wrong PSRAM mode fails before that, at boot.

3. **The agent answers.**

   ```
   qjsx device info --target serial:COMx@921600
   ```

   Expected, on a board with nothing installed:

   ```
   qjsx: connected to qjsx esp32s3 (QuickJS <version>) on serial:COMx@921600
   running:  false
   package:  (none) (0 bytes)
   ```

   The first line (on stderr) shows `agent::Identity`: the name and the engine version. The
   flash and PSRAM sizes of `board::identity()` are not shown: the agent does not send them
   yet.

4. **A package deploys and runs.** Build a small program, for example the fixture
   `tests/fixtures/nocompiler` (language features only, it ends by itself), then send it:

   ```
   qjsx build tests/fixtures/nocompiler
   qjsx deploy <package> --target serial:COMx@921600 --monitor --until-exit
   ```

   `qjsx build` prints the path of the package (`built <file>`), and a warning for the `eval()`
   this fixture contains on purpose. Expected on stderr during the deploy: `installed ...`,
   `program started (build ...)`, then `program ended (code <n>)`; the output of the program
   is on stdout. The same package can be run on the PC with
   `bin/Release/qjsx-target run <package>` to compare the output (on both, the line that
   starts with `eval` says that eval is not supported: there is no compiler). After this step
   `qjsx device info` shows the build id and the size of the package, and the program starts
   again at each boot.

5. **The debugger stub works.**

   ```
   qjsx run tests/debugger.test.js serial:COMx@921600
   ```

   The test builds `tests/fixtures/debuggee`, installs it on the board (it replaces the
   installed package) and debugs it through the agent: breakpoints, stepping, pause of a
   running loop, change of a variable, exception, `debugger;`. Expected last line:
   `debugger: 52/52 checks passed`.

Worth trying once more on a new port: `qjsx device stop` and `qjsx device reset`; a deploy
while a program loops; a package built by another version of the tool, which the device must
refuse with a message that names both bytecode versions.

Symptoms met so far:

| Symptom | Cause |
|---|---|
| The chip fails at boot on a board with PSRAM | wrong PSRAM mode (octal / quad) in `sdkconfig.board` |
| `qjsx: no "packages" partition` | the partition table has no `packages` partition of type `data`, subtype `0x40` |
| `task_wdt ... IDLE1` every 5 s while a program computes | `platform::yieldIfDue()` does not pause the JS task, or native code spins |
| The tool gets no answer on a serial target | the baud rate is missing from the target (`@921600`) |
| "the package holds bytecode version N, this device reads version M" | firmware and tool were not built from the same sources: rebuild the package with the matching tool, or flash a matching firmware |
| "cannot store a package of this size" | the package is larger than a slot minus its header, or the slot it needs is the one the running program is mapped from |
| A new source file is not compiled, or a removed one is still asked for (ESP-IDF) | ninja kept the old file list: `reconfigure build` |

## 6. Not done yet on any device

- **File system.** `fs` fails with `ENOSYS` and `process.cwd()` is `"/"`. The `data` partition
  of the ESP32 tables is reserved for it.
- **Hardware modules** (GPIO, PWM, I2C, SPI, UART, ADC, WiFi...). A program can compute, print
  and use timers; it cannot drive a pin.
- **Native USB and WiFi links.** The only link is UART0 on the ESP32.
- **OTA** (firmware update over the link). The 8 MB and 16 MB tables reserve two firmware
  slots; nothing uses them.
- **The device identity in the protocol.** `board::identity()` is implemented and not
  transmitted.
- **Several links on one board**, and events posted from interrupt handlers.
- **The RP2350 port.** Planned for the Olimex PICO2-XXL, with FreeRTOS and the pico-sdk. No
  file of it exists.

## 7. Files to read

| File | Why |
|---|---|
| `common/runtime/platform.h` | the system interface, with the contract of each function |
| `common/board/board.h` | the board interface |
| `common/agent/agent.h` | `Link`, `Storage`, `Identity`, and the agent |
| `common/agent/runner.h`, `common/agent/runner.cpp` | `agent::runDevice()` and the runner |
| `ports/cmake/qjsx.cmake` | what every port builds |
| `ports/freertos/platform_freertos.cpp` | `platform.h` on FreeRTOS: sleep, yield, mutex, signal |
| `ports/esp32/main/` | the ESP32 port: entry point, board, platform |
| `ports/esp32/CMakeLists.txt`, `ports/esp32/sdkconfig.defaults`, `ports/esp32/boards/`, `ports/esp32/partitions/` | the per-board build |
| `host/platform/board_host.cpp`, `host/platform/platform_host.cpp` | the desktop implementation of the same interfaces |
| `tools/idf.ps1` | how the ESP-IDF build is launched on the author's machine |
| [architecture.md](architecture.md), 5.5 | the design and the decisions |
| [engine-changes.md](engine-changes.md), H and I | the engine without compiler; the changes for newlib |
| [known-bugs.md](known-bugs.md) | defects found and not fixed |
