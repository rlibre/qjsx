# History of fixed defects

A fixed defect leaves `known-bugs.md` and arrives here, with what was done. The most recent
one comes first.

## 2026-11-11

### Answer to `RESET` lost on the ESP32

- **Defect.** The agent handed the answer to `UartLink::send`, then `board::restart()`
  called `esp_restart()` at once: `uart_write_bytes` only copies the frame into the transmit
  buffer of the driver, so the chip could restart before the answer left, and the host waited
  for it in vain.
- **Fix.** `board::restart()` (`ports/esp32/main/board_esp32.cpp`) waits for the end of the
  transmission (`uart_wait_tx_done`, at most `RESTART_FLUSH_MS` = 100 ms) before
  `esp_restart()`.
- **Verified.** On the board: `qjsx device reset` answers ("reset done").

### `onData` callback freed while it runs (`net`, `serial`)

- **Defect.** The `net` and `serial` modules called the `onData` callback straight from
  their member. A callback that replaces itself (`connection.onData(...)` inside the
  callback, as the WebSocket handshake does when the socket takes the connection over)
  freed the running function: the interpreter then read a freed closure (segmentation
  fault, found by `tests/websocket.test.js`).
- **Fix.** Both call through a local copy of the callback, which keeps the function alive
  until the call returns, as `onClose` already did (`host/modules/net.cpp`,
  `host/modules/serial_module.cpp`).
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
  flag).
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
2. **`serial`, `SerialPort::write` after `close()`** (`host/modules/serial_module.cpp`).
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
(`pthread_condattr_setclock`, `poll`, `fseeko`, `MSG_NOSIGNAL`...). Step over / step out is
not fixed: it stays in `known-bugs.md`.

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
