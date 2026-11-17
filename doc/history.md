# History of fixed defects

A fixed defect leaves `known-bugs.md` and arrives here, with what was done. The most recent
one comes first. The section "Measures and findings" at the end keeps the results of
measurements that changed no code but settle a question.

## 2026-11-17

### Defects of `stdlib/display` found while writing its typing

Found by the agent that wrote `display.d.ts` (it only reported them); fixed and checked on
the twin of the WT32-SC01 (temp/displayfix) and by `tests/display.test.js` (42/42).
- A `show()` whose native part threw left the display "sending" for good: every later
  `show()` resolved false. It also threw instead of rejecting (rule 2 of the errors of the
  JS API), as did a `show()` after `close()`. `show()` now never throws: a failure rejects
  and the display is free for the next image (`#startShow()`).
- `text()` with something else than a string (a number) made the list's length NaN and
  corrupted the list until the next `show()`. The value is now written as it would print
  (`String(value)`): `lcd.text(10, 10, 21.5, font, color)` works.
- The constructor left the SPI bus (and the reset pin) taken when a later step failed (a
  backlight pin refused, a controller command): the port stayed EBUSY until the end of the
  program. A failing step now closes what was opened before throwing.
- Stale comments: the kinds 3 and 4 of a command list in `draw_module.cpp`, "a buffer" of a
  band where there are two, an error message naming an Int16Array where any typed array is
  read.
- No type for a file imported as bytes: the editor said "Cannot find module './x.bin'". New
  `common/runtime/modules/bytes_import.d.ts` declares `"*.bin"` (a `Uint8Array`); the drift
  test leaves wildcard declarations out.

### The twin rebuilt its whole SPI trace every 50 ms with no page open

Found by the agent that brought the WT32-SC01's screen into the twin, while measuring its
CPU use. When no page is open, the page's server asks for the trace records after
4294967295 to skip them all; `TraceRing::visit` (host/twin/sim/twin.h) computed `after + 1`
on 32 bits, which wrapped to 0, so every 50 ms it made all 256 kept SPI records into JS
objects and dropped them: 8 to 12 % of a core once the trace was full. Now computed on 64
bits; the twin is idle at 0 % after a reset.

### Small defects of the coding rules pass and older ones

Fixed in the code without a build nor a test run by the agent that made them; built and
tested afterwards with the rest of the day's work.

- **Twin: `Registers` did not wrap a written register index** (a part with fewer than 256
  registers in "pairs" mode) -> `_write` and the register pointer wrap around, as reads
  do. `host/twin/api/twin.js`.
- **`device/rmt`: `hal::rmt::close(-1)` when the open failed** -> the hal is given back
  only a channel it opened (one past the module's table). `common/device/rmt_module.cpp`.
- **ESP32 `device/pwm`: result of `setUpTimer()` ignored in `setFrequency`** -> a timer
  LEDC refuses undoes the move (`undoMove`: the channel back on its timer, at its former
  frequency) and the call fails with EIO (documented in `hal_pwm.h`, `pwm_module.cpp`,
  `pwm_module.d.ts`). `ports/esp32/main/hal_pwm_esp32.cpp`.
- **ESP32 `device/adc`: `deleteCalibration` assumed the curve fitting scheme** -> each
  input keeps the scheme that made its calibration (`Calibration { handle, scheme }`) and
  is deleted by it; a channel whose configuration fails no longer leaves a unit with no
  user (deleted when it was made for that input). `ports/esp32/main/hal_adc_esp32.cpp`.
- **`device/serial` (board path): `JS_ToFloat64` on any argument** -> only a number is
  converted (no `valueOf` runs). `common/device/serial_module.cpp`.
- **jsbind: a failed `JS_ToCStringLen` (`JS_ENUM`) or `JS_AtomToCString`
  (`rejectUnknownFields`) threw a TypeError over the pending exception** -> the pending
  exception is passed on (`markPendingException`). `common/jsbind/jsbind_convert.h`.
- **Teensy 4.0: default `thread_local` block made without a lock** -> made once with the
  scheduler suspended (tasks without a block of their own: idle, timer); read without it
  once it exists. `ports/imxrt1062/main/tls_imxrt1062.cpp`.
- **Teensy 4.0: `board::restart()` without `return`** -> group comments, and why none is
  needed (`NVIC_SystemReset()` is `__NO_RETURN` in CMSIS). `ports/imxrt1062/main/board_imxrt1062.cpp`.
- **Desktop agent: `offset + size <= _uploadSize` could wrap around** -> compared without
  the addition (`offset <= _uploadSize && size <= _uploadSize - offset`).
  `host/platform/board_host.cpp`.
- **Debugger: `hitBreakpoints` compared lines only** -> a breakpoint is hit when the top
  frame is in its function at one of its offsets (`breakpointLocation` of the session also
  gives `number` and `pcs`). `host/js/cdp.js`, `host/js/debugsession.js`.
- **RP2350: `board::confineFiles()` returned false** -> true, as the other boards (no
  effect while `dataDirectory()` is nullptr). `ports/rp2350/main/board_rp2350.cpp`.
- **Agent upload hash allocated with `new` / `delete`** -> `memory::allocate` /
  `release`, one block made at the first upload and reused. `common/agent/agent.cpp`.
- **`readAll` returned EIO when measuring the size failed** -> the errno of the seek (or
  the tell), kept across `fclose`; EIO only if none was set. `common/runtime/files.cpp`.
- **BigInt (debugger values): `strtoll` accepted leading spaces and a `+`** -> a digit
  must come first, after an optional `-`. `common/agent/debugger_values.cpp`.
- **Arrays of more than 256 bytes on the stack** -> the 512-byte messages of `js::fail`,
  `failSystem`, `reportException`, `Resolver::reject` and of the EACCES message of `fs`
  on the heap (`js::detail::MESSAGE_SIZE`, same truncation); the paths of `data_files.cpp`
  on the heap (`addEntry`, the recursive `addUsage`, `resolve`); SHA-1 keeps its message
  schedule in 16 words (FIPS 180-4, 6.1.3) instead of 80. `common/jsbind/jsbind_error.h`,
  `jsbind_promise.h`, `common/runtime/modules/fs.cpp`, `common/agent/data_files.cpp`,
  `common/runtime/sha1.h`.

### A request of a new session answered with an answer of the previous one (USB links)

- **Defect.** The agent keeps its last answer (and the debugger's) with the number of its
  request, to send it again when the same number comes back (an answer lost on the way).
  It forgot them only when a new host connected (`Agent::attach`). On a USB link (RP2350,
  Teensy 4.0) the link outlives the sessions of the tool, and every session numbers its
  requests from 1: a request of the next session that fell on a kept number got the old
  answer and was not run. Found on the RP2350: after a session HELLO (1), DEBUG_ATTACH (2),
  DEBUG_DETACH (3), the UPLOAD_BEGIN (3) of the next session got the answer of
  DEBUG_DETACH, and UPLOAD_CHUNK answered "no upload in progress". The ESP32 escaped it:
  opening its serial port restarts the chip.
- **Fix.** A HELLO opens a session: `Agent::onMessage` forgets the kept answers
  (`Agent::forgetAnswers`, under the agent's mutex) before the check for a repeated
  request, and a HELLO is always run (running a repeated HELLO again is harmless).
  `common/agent/agent.cpp`, `agent.h`. Checked on the RP2350: a debugger session, then the
  proxy suite: 33/33.

### The RP2350 hung after a few uploads (pico_sync interop of the FreeRTOS port)

- **Defect.** After a random number of sessions (4 to 6 uploads, or a debugger session
  then two others) the board stopped: no answer, USB port stuck on the PC. Measured with a
  firmware built with `QJSX_FLASH_TRACE` (watchdog of 3 s fed by a task of core 0, records
  kept across its restart): every flash operation had completed (`flash_safe_execute`
  returned 0), no hard fault, no stack overflow (USB reader 636 words free of 1024, JS task
  11131 of 12288), the USB reader was waiting; when the feeder stopped, core 0 ran its idle
  task although the feeder was ready, and core 1 ran the FreeRTOS timer task ("Tmr Svc")
  for more than 2 s: the scheduler stayed suspended.
- **Cause.** With `configSUPPORT_PICO_SYNC_INTEROP` (FreeRTOS-Kernel V11.3.3, community
  port `GCC/RP2350_ARM_NTZ`, `port.c`), every pico_sync object of the SDK shares one event
  group, and a release from an interrupt (`vPortLockInternalSpinUnlockWithNotify` ->
  `xEventGroupSetBitsFromISR`) is deferred to the timer task. Our USB callback released a
  pico_sync semaphore from the USB interrupt at each received block; the SDK's stdio also
  uses pico_sync. The exact place where the timer task blocks was not found; the path is
  the port's. REPRODUCED outside qjsx (`temp/freertos-repro/`, about 200 lines: ISR
  `sem_release` + task `sem_acquire_timeout_ms` + `stdio_get_until` on core 0, `printf`
  on core 1, `flash_safe_execute` in a loop, host sending): hangs in 2-3 s with the
  interop; no hang without the flash loop, without host traffic, or with the interop off
  and task notifications. Draft issue for FreeRTOS-Kernel-Community-Supported-Ports:
  `temp/freertos-port-issue.md` (not posted).
- **Fix.** `ports/rp2350/main/board_rp2350.cpp`: the USB callback wakes the reader by a
  FreeRTOS task notification (`vTaskNotifyGiveFromISR`, `ulTaskNotifyTake` with the same
  20 ms); `FreeRTOSConfig.h`: `configSUPPORT_PICO_SYNC_INTEROP` 0 (the SDK's mutexes wait
  actively, briefly). Checked: 24 proxy suites in a row, then debugger / proxy / cdp twice
  without restart (72/72, 33/33, 53/53), then 8 proxy suites on the normal firmware.
- **Kept**: the measure `QJSX_FLASH_TRACE` (CMake option of the RP2350 port, OFF): watchdog,
  last flash step, USB reader phase, hard fault PC / LR, least free stacks, tasks running
  on each core when the feeder stopped, printed after the restart.

### The RP2350 firmware did not build

- **Defect.** `RESTART_FLUSH_MS` was defined both by `common/board/board.h` (since
  `device/board`, 2026-11-13) and by `board_rp2350.cpp`; `board::dataCapacity()` and
  `platform::randomBytes()` were missing.
- **Fix.** The port's constant renamed `USB_FLUSH_DELAY_MS` (100 ms, unchanged);
  `dataCapacity()` returns 0 (no file system); `randomBytes()` draws from `pico_rand`
  (seeded by the TRNG of the RP2350; `pico_rand` linked). Flashed: debugger 72/72, proxy
  33/33, cdp 53/53 on a board just restarted.

## 2026-11-16

### Unclear report of an out of memory, and errors reported twice

- **Defect.** Out of memory, the engine throws `null` when it has no room left to make
  the error (`JS_ThrowError2`): the program printed "Uncaught (in promise) null". An
  error made out of memory has no backtrace, and its `stack` (null) was printed as
  "null" glued to the next line; a text that could not be made printed "(exception)".
  An error thrown by the main module itself was printed twice: it rejects the promise
  of the module's evaluation and the module's own, with the same reason.
- **Fix.** `js::reportException` (`common/jsbind/jsbind_error.h`) says "null (out of
  memory? ...)" for a thrown null, "an exception whose text could not be made: out of
  memory?" when the text fails, and prints the stack only when it is a string.
  `Loop::reportRejections` (`common/runtime/loop.cpp`) skips a rejection whose reason (an
  object, or null) is the one just reported.
- **Verified.** By hand: a module that throws prints once; a loop that fills a 1 MB
  heap prints the out of memory line once. Tests in Debug.

### Timers of the PC 15.6 ms late (Windows)

- **Defect.** `platform::monotonicMs()` read `GetTickCount64`, which moves by steps of
  15 to 16 ms. A timer of the loop due in 1 to 10 ms woke up on time (the wait itself is
  precise), saw the clock unchanged, and expired only at the next step: every
  `setTimeout(1..15)` lasted about 15.6 ms on Windows (qjsx run, desktop agent, twin), and
  so did a send of the twin's RMT (its end is computed with the same clock). Found when
  the status LED of the client board's twin breathed in 1.5 s instead of 1 s.
- **Fix.** `monotonicMs()` reads `QueryPerformanceCounter` (`host/platform/platform_host.cpp`;
  19 ns per read instead of 1.4 ns, a few reads per turn of the loop).
- **Verified.** `setTimeout(10)` 15.53 -> 10.40 ms, `setTimeout(1)` 15.60 -> 1.46 ms;
  the twin's status LED breathes in 1.000 s; tests in Debug. High resolution waitable
  timers were measured too: not needed on this PC (a timed wait of 10 ms already lasts
  10.4 ms), kept aside in case a PC waits by steps of 15.6 ms.

### Peak of the JS region counted since the boot

- **Defect.** `RegionHeap` kept its peak since `init()`, and the ESP32 reserves the JS
  region once per boot. The memory map (`__memoryMap`, `QJSX_MEMORY_MAP`) printed as "peak"
  the most used by every program since the boot: a map taken while the client board's
  application was loading showed a 211 KB peak for 166 KB used, read as a 45 KB transient
  of the loading, when it was the previous run of the same application (≈ 211 KB once
  running). On the twin, the peak of a run equals what is used right after the loading.
- **Fix.** The peak starts again when the heap was empty (`RegionHeap::countUsed`,
  `common/runtime/region_heap.cpp`): the JS region serves one runtime at a time and a run
  frees every block, so the peak is the current run's. The desktop heap
  (`QJSX_JS_HEAP_MB`), shared by several runtimes, is never empty: its peak still counts
  from the start of the process.
- **Verified.** `tests/region_heap.test.js` (a block in an empty heap starts a new peak),
  tests in Debug. Not tried on a board.

### Twin: the parts of the project not found without `-p`

- **Defect.** `qjsx agent` run in the folder of a project (no `-p`) has the project folder
  ".", so the model of a part `"./twin/supply"` was imported as
  `"././twin/supply/model.js"`: `import()` resolved it from the importing module,
  `qjsx:twin/parts`, and failed ("could not load module 'qjsx:twin/twin/supply/model.js'").
  With `-p` and a path without "./" the name was read from the disk as is, so the tests
  did not see it.
- **Fix.** `modelFolder()` (`host/twin/js/parts.js`) gives an absolute folder
  (`resolve` of `stdlib/path`).
- **Verified.** The twin of the client board's application starts from its folder, page
  tried in the browser pane; new check in `tests/run-tests.sh` (the twinparts project run
  from its folder), tests in Debug.

## 2026-11-11

### Answer to `RESET` lost on the ESP32

- **Defect.** The agent handed the answer to `UartLink::send`, then `board::restart()`
  called `esp_restart()` at once: `uart_write_bytes` only copies the frame into the transmit
  buffer of the driver, so the chip could restart before the answer left, and the host waited
  for it in vain.
- **Fix.** `board::restart()` (`ports/esp32/main/board_esp32.cpp`) waits for the end of the
  transmission (`uart_wait_tx_done`, at most `RESTART_FLUSH_MS` = 100 ms) before
  `esp_restart()`. Since `device/board` (2026-11-13), `RESTART_FLUSH_MS` is 500 ms, in
  `common/board/board.h`, and also covers the open UARTs and USB serial functions.
- **Verified.** On the board: `qjsx device reset` answers ("reset done").

### `onData` callback freed while it runs (`net`, `serial`)

- **Defect.** The `net` and `serial` modules called the `onData` callback straight from
  their member. A callback that replaces itself (`connection.onData(...)` inside the
  callback, as the WebSocket handshake does when the socket takes the connection over)
  freed the running function: the interpreter then read a freed closure (segmentation
  fault, found by `tests/websocket.test.js`).
- **Fix.** Both call through a local copy of the callback, which keeps the function alive
  until the call returns, as `onClose` already did (`host/modules/net.cpp`,
  `host/modules/serial_module.cpp`; the serial module is now
  `common/device/serial_module.cpp`, which keeps the local copy).
- **Verified.** `tests/websocket.test.js`, tests in Debug.

### Minor defects with a local fix

Fixed together, after the review of `new-device.md` (user: "the small cleanups"):

- **Desktop `startLinks()`**: printed nothing when the accept thread could not start; it
  now says so on stderr, as `board.h` asks.
- **ESP32 `writeConsole()`**: compares with `rt::OUTPUT_STDERR` instead of the literal `2`.
- **`manifestMain`**: a "main" that `JS_ToCString` cannot convert (out of JS memory) no
  longer says "manifest.json has no main".
- **POSIX `serial`**: `tcgetattr` is checked; one stop bit and no RTS/CTS are set
  explicitly after `cfmakeraw` (`CRTSCTS` where the system has it); a read or a write
  interrupted by a signal (`EINTR`) goes on instead of closing the port. Not compiled (no
  POSIX build here).
- **Windows `listPorts`**: a registry value too long for the buffers (`ERROR_MORE_DATA`) is
  skipped; the list goes on.
- **CBOR `Reader::integer`**: an integer outside the `int64_t` range is refused instead of
  wrapping around to a negative number.
- **Agent `Fields`**: a payload with more than 16 pairs is malformed (the request is
  ignored, like any malformed message) instead of being half read.
- **CBOR `Writer::append`**: appending 0 bytes no longer calls `memcpy` with the null buffer
  of an empty writer.
- Also: stale header comments of `board.h`, `platform.h`, `runner.h` and `agent.h`
  (`Storage` threads, who gives the `Controller`), and `QJSX_NO_COMPILER` listed twice in
  `ports/cmake/qjsx.cmake`.
- **Verified**: tests in Debug on the PC. Not built for the ESP32 yet.

### Step over / step out stopping in unrelated code

- **Defect.** The debugger stub compared frames by the address of their `JSStackFrame` and by
  the depth of the stack. A step "over" the end of `first()` in `add(first(), second())`
  stopped in `second()`, called next from the same place of the C stack (same address). A step
  "over" an `await` stopped in a timer callback that ran meanwhile (shallower stack).
- **Second defect, found by the new test.** `core/debug.c` described the frames of async
  functions and generators as native frames (no module, number, pc nor slots): it only
  recognized the class `JS_CLASS_BYTECODE_FUNCTION`.
- **Fix.** An activation number per frame, given by the engine on demand, unique and never
  reused (`JSDebugFrame.id`), and `JSDebugFrame.is_async` ([engine-changes.md](engine-changes.md),
  L). The stub records the start frame and its callers when a step starts, waits for an
  `async` start frame, and ends the step at the next statement run when no recorded frame is
  left, as V8 ([architecture.md](architecture.md), 3.3). `js_class_has_bytecode()` in
  `core/debug.c` for the other defect.
- **Verified.** `tests/debugger.test.js` (72 checks, fixture `tests/fixtures/debuggee`: end
  of `first()`, `await` with a timer, step out of an async function resumed by a promise job),
  tests in Debug on the PC. Not yet: Release, original QuickJS tests, microbench, board.

### Very slow serial port on Windows

- **Defect.** `qjsx deploy` took 16 to 26 s to send 47 KB to the board at 921600 baud, and
  one attempt exceeded 60 s. Writing a single byte took from 51 ms to 6 s, always in steps of
  about 100 ms. A request could exceed its 3 s timeout and be sent again.
- **Cause.** In `host/modules/serial_module.cpp`, the port was opened in synchronous mode
  (`CreateFileA` without `FILE_FLAG_OVERLAPPED`). The reader thread (`readLoop`) stayed
  blocked in `ReadFile` for up to 100 ms (`COMMTIMEOUTS`), then started again at once. On a
  synchronous handle, Windows lets the operations through one by one: the `WriteFile` of the
  JS thread waited for the end of the `ReadFile` in progress, and the reader often got ahead
  of it again. During a send, the board waits for the request and emits nothing, so every
  read went to the end of its 100 ms. The board and the protocol were not at fault.
- **Fix.** The port is opened with `FILE_FLAG_OVERLAPPED`; every read and every write has its
  `OVERLAPPED` structure and its event (`beginOperation`, `endOperation`). Reads and writes
  are independent. The rest does not change (read that returns after 100 ms, `closing`
  flag). Since `device/serial` (2026-11-11), this Windows code is the PC driver
  `host/platform/serial_host.cpp`, under `common/device/serial_module.cpp`.
- **Verified.** On the board: a one-byte write takes 0 to 1 ms; sending 47 KB goes from
  16–26 s to 1.5 s (50 ms per 2 KB chunk, regular); the debugger passes 52/52 in 1.7 s. PC
  tests in Release and in Debug.

### Debugger test with a program already running

- **Defect.** `tests/debugger.test.js` failed if a program was already running on the device
  when it connected (seen on the board, with `debuggee` installed and its `spin` loop
  running). Its initial `STOP` stopped this program, hence an `EXITED` event; the test then
  waited for `STARTED` and received this `EXITED` ("program started: expected 32, got 31"),
  then everything was shifted.
- **Fix.** `expectStarted()` skips the `EXITED` events that precede the expected `STARTED`:
  the device always ends a program before launching the next one, so the `EXITED` of the old
  one necessarily arrives before. Any other event makes the check fail.
- **Verified.** PC tests in Release and in Debug; on the board, 52/52 with a program running
  at the time of the connection.

### Watchdog during a long computation (ESP32)

- **Defect.** A JS program that computed without yielding starved the idle task of core 1:
  ESP-IDF printed "task_wdt ... IDLE1" every 5 s.
- **Fix.** `platform::yieldIfDue()`, called by `Runner::interrupt`: a pause of one tick every
  100 ms. Explanations in `architecture.md` 5.5.
- **Verified.** On the board, no "task_wdt" in 17 s during the `spin` loop.

### "Serious" defects recorded by the agents that documented the code

The tests of `run-tests.sh` pass in Release and in Debug, but no test was written for these
defects in particular.

1. **`fs.rmSync` with `recursive` followed symbolic links and junctions**
   (`common/runtime/modules/fs.cpp`, `removeTree`).
   - Defect: it used `files::stat`, which follows links. A link to a folder inside the tree
     made the content of the target be deleted. Node uses `lstat` and only removes the
     link.
   - Fix: `removeTree` uses `files::linkStatus` (lstat; Windows junctions and links through
     the reparse point attribute). A link is removed itself, never the content of its
     target.
2. **`serial`, `SerialPort::write` after `close()`** (`host/modules/serial_module.cpp`, now
   `common/device/serial_module.cpp`: `PortState`, `releasePortState`).
   - Defect: `_state` was `nullptr` while `_open` was still true, hence a `nullptr`
     dereference. After a read error, the reader thread freed `PortState` while the JS still
     pointed to it: `write()` and `requestClose()` used freed memory.
   - Fix: `PortState` is reference-counted (JS side + reader thread). The last one closes the
     port and frees the state; `write` refuses a port whose state the JS has dropped.
3. **CBOR: bounds that overflowed** (`common/agent/cbor.cpp`).
   - Defect: `Reader::text`, `Reader::bytes` and `skip` (case of strings) tested
     `_position + argument > _size` with a 64-bit length coming from the link. A huge length
     made the test overflow, hence an out-of-bounds read; on 32 bits, the `(size_t)` cast
     also truncated the length. `skip` on a map computed `argument * 2`, which could
     overflow. The recursion of `skip` had no depth limit.
   - Fix: `fits()` bounds any length without overflow; `skip` is limited to 16 levels; maps
     are walked by pairs.
4. **Malformed zip package** (`common/runtime/package.cpp`, `Reader::open`).
   - Defect: the lengths of the name, of the extra and of the comment were not bounded by the
     end of the directory; on 32 bits, the additions of offsets could overflow. Result:
     out-of-bounds reads, including in `find()` (`memcmp`). After a failed `open()`, the
     entries already read stayed in the table.
   - Fix: 64-bit offsets and `within()`; entry name bounded; pointers computed after the
     check; table emptied after a failure.
5. **The common registry registered the `net` module**, which only exists on the host
   (`common/runtime/modules/modules.cpp` referenced `jsNewModule_net`).
   - Defect: the firmware would not have linked.
   - Fix: `net` is registered by `host/main.cpp`, no longer by the common registry.
6. **Size computation overflows in the stub** (`common/agent/debugger.cpp`).
   - Defect: the multiplications by a value coming from the network could overflow on
     32 bits, hence a buffer too small and a write outside the heap (`handleSetStops`:
     `(size_t)count * 4` and `sizeof(uint32_t) * (count + 1)`); in `handleCall`,
     `argumentCount` was not bounded; the `malloc` was not checked.
   - Fix: the number of offsets is compared with what remains before any multiplication;
     `CALL` is limited to 256 arguments.

### Allocations never checked

- **Defect.** `malloc` / `realloc` never checked (stub, `receiveBlock`, `registerName`...).
- **Fix.** All our code allocates through `memory::allocate` / `reallocate` / `release`
  (`common/runtime/memory.h`). Running out of memory is fatal there (`platform::fatal`), so
  the unchecked `malloc`s no longer exist. Tests OK on 2026-11-11.

### "Medium" defects

The tests of `run-tests.sh` pass in Release and in Debug, but no test was written for these
defects in particular. The POSIX / macOS branches were only reread, not compiled
(`pthread_condattr_setclock`, `poll`, `fseeko`, `MSG_NOSIGNAL`...). Step over / step out was
not fixed in this pass: it was fixed later the same day (see "Step over / step out stopping
in unrelated code" above).

- **Runner and debugger**:
  - STOP and DEBUG_ATTACH during startup;
  - PAUSE lost after SCRIPT_LOADED;
  - `hostGone` (`followHost`);
  - repeated requests (kept in the queue while they execute, last answer separate for
    agent / debugger);
  - double lock of `onMessage` (`resendAnswer`);
  - `attach` (`forgetAnswer`);
  - `_requestDepth` guard in `writeObject`;
  - truncated SET_STOPS (error, nothing applied);
  - mode of SET_EXCEPTIONS bounded;
  - `runDevice` resets the exit;
  - `formatDate` without `gmtime` (Hinnant's civil days);
  - invalid BigInt;
  - `flushSegment` without a copy when there is no text callback.
- **Runtime and platform**:
  - rejection of the last timer displayed;
  - `net`: socket state reference-counted, `closing` + `shutdown`, threads that wait with
    `waitReadable`, failure to start a thread, `connectDone`;
  - `MSG_NOSIGNAL` / `SO_NOSIGPIPE`;
  - link of the agent without a reader (`endLink`);
  - `finishUpload` (order described in the code);
  - recursive `Mutex` everywhere;
  - `Signal::wait` on a monotonic clock;
  - `files.cpp`: file on the path, `errno`, files of more than 2 GB, empty drive root,
    `readdir` error, `rename`;
  - timer delays (NaN, negative, too large: 1 ms like Node);
  - `fs`: `writeFileSync`, `encoding` getter.
- **jsbind**:
  - `Resolver` already settled or empty;
  - `js::fail` of the constructor in `js::create`;
  - `JS_EXCEPTION` argument in `callWith`;
  - calls outside a JS call (`detail::propagateException`);
  - `registerName` (deduplicated names, `thread_local`, 32768 at most);
  - exact bounds 2^63 / 2^64;
  - `JS_NewObject` checked;
  - `static_assert` beyond 64 `JS_EXPORT`.

### "Minor" defects

- **`process.exit` did not free the runtime**: it now ends the program, no longer the
  process ("process.exit" tests of `run-tests.sh`).
- **`setImportMeta` kept the `\` of Windows paths** in the `file:///` URL: fixed by
  `files::useSlashes`.

## Measures and findings

Results taken out of `TODO.md`: measurements and conclusions recorded nowhere else.

### Drawing on a display from JS, on the ESP32 (WT32-SC01, 2026-11-17)

Measured while designing `stdlib/display` (architecture.md 5.4); the program for each is
in `temp/` (wtscreen, spibench, wtdisplay, wtcanvas, wtdraw, wtshow, wtgauge).
- A call from JS to native code costs about 0.25 ms (JS heap in PSRAM): drawing at once,
  with one SPI call per primitive, gave 2.7 ms a pixel and 1 s for a line across the screen.
- `transactionSync` of one segment of 2 bytes 1.3 ms, each segment 0.35 ms more; an array
  of numbers instead of a typed array 0.45 ms more (suspected: the conversion of each
  segment object; not proven).
- A canvas of the screen (307 KB in PSRAM): 82 ms to fill, 92 ms to send; 3.6 images a
  second drawn in JS, 4.4 with native primitives. A list of commands rendered band by
  band: 8.1, then 13.5 when the next image is listed during the send.
- The ST7796 shows nothing at 80 MHz; 40 MHz works (61 ms for a full screen of wire).
- Fixed point with 64-bit integers is no faster than float on the ESP32 (a 64-bit
  multiply or compare is a library call); with 32-bit integers the arcs went from 37 to
  19 ms an image. The details are at the top of common/draw/draw.cpp.
### Idle GC: useless for the client board's application

- **Question.** Would a GC pass run while the loop waits (the user's idea) help a board
  without PSRAM?
- **Measure.** The client board's application, copied into a probe project, run on the
  board with the `__gc()` probe of the measure option `QJSX_MEMORY_MAP`: the JS heap stayed
  at 200.8-201.6 KB for 30 s, and NO automatic pass ran (everything is freed by the
  reference counts: the application makes no cycles). A forced pass took about 3 ms and
  freed nothing.
- **Conclusion.** Nothing to gain for that application; to reconsider for a program that
  makes cycles.

### JS region against the shared heap, measured again with the flash in QIO

- **Measure.** `temp/allocfull` and `temp/allocbench` on the client board (ESP32-S3 without
  PSRAM), flash in QIO, logs `temp/alloc*-qio.log`; times region / shared heap: compute
  1263 = 1263 ms, arrays equal, closures 553 / 546 ms (the +85 % measured in DIO is gone:
  it was the flash), JSON +8 %, properties +24 %, objects +24 % (in DIO the region was 9 %
  faster: noise of the code placement), string building +64 % (the same in DIO and QIO):
  strings of 400 and 2400 bytes x2.2 and +64 %, strings of 100 bytes equal.
- **Lead.** Blocks of more than 128 bytes go to the `RegionHeap`, whose code runs from the
  flash behind the 16 KB instruction cache, while ESP-IDF's heap (the shared heap) runs
  from IRAM. Trying the `RegionHeap` in IRAM (a low level setting) is postponed by the user
  (TODO.md).

### Speed of the client board after the package executed in place

- **Measure.** `temp/allocbench` on the client board, firmware with the package executed in
  place (engine change S) against the firmware just before: identical to the millisecond
  (compute loop, arrays, strings, small objects), except the creation of functions in a
  loop, twice slower (532 -> 1146 ms for 20,000). The benchmark's package was in RAM, so
  the flash is not the cause. Not proven; the lead is still the 16 KB instruction cache
  (the place of the code changes from one firmware to the next).

### Flash in QIO on the ESP32-S3 boards

- **Done.** `CONFIG_ESPTOOLPY_FLASHMODE_QIO` in the `sdkconfig.board` of the client board,
  of the YD-ESP32-S3 and of the ESP32-S3-DevKitC-1 N8R8. On the client board the waits
  got about 1.6 times faster. A `toggle()` loop on GPIO20: 244 kHz in DIO as in QIO (the
  interpreter loop stays in the cache).

### Memory map of the client board

- **Measure option** `-DQJSX_MEMORY_MAP=ON` (global `__memoryMap(title)`), raw output in
  `temp/histo/map-clean.txt`. Static RAM of the firmware: 72.6 KB (code in RAM 43.5 KB,
  `.data` 16.5 KB, `.bss` 12.6 KB).
- **Three pockets of RAM found**: (1) the JS task's stack of 48 KB, 41.3 KB never used
  (the board's stack is 32 KB since, which gave 16 KB more to the JS region); (2) a 32 KB
  heap at 0x3fcf0000 and the 7.8 KB RTC heap, entirely free, which the JS region (one
  block of the main heap) cannot take: not used yet (TODO.md); (3) a "211 KB peak for
  166 KB loaded", which was a measure artifact (history.md "Peak of the JS region counted
  since the boot").
- **On the twin** (PC, 64-bit values): the peak of a run equals the heap used just after
  loading (284.2 KB), then 300-304 KB while running (316 KB at most); the 45 KB between
  "loaded" and "running" are the live memory of the application (about 57 async functions
  waiting, 102 closures, 313 objects, 55 shapes, 4 ArrayBuffers). An earlier measure of the
  same application on the twin gave 380 KB, 400 KB at most (before the engine's memory
  work).

### Trial projects of the twin page

- `temp/scopedemo` (`qjsx agent -p temp/scopedemo`): the analyzer; `temp/pwmdemo`: four PWM
  outputs and a turning RGB LED, deployed on the agent of `temp/togglestress`;
  `temp/togglestress`: an endless `toggle()` loop: the page does not saturate, but
  `twin.state` is sent at each turn, 127 KB/s (TODO.md); `tests/fixtures/twinparts`: the
  parts.
