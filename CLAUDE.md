# qjsx

JavaScript runtime based on QuickJS (Fabrice Bellard), targeting desktop (Windows, macOS, Linux)
and ESP32 with PSRAM. Current work: a QuickJS debugger usable from VS Code.

Name (user, 2026-11-11): the project, the tool and its command are called `qjsx` (formerly
`espjs`): `qjsx build`, `qjsx.json`, embedded modules `qjsx:name`, engine comments marked
`qjsx:`, defines `QJSX_*`. The engine keeps its folder `engine/qjsx`; its libraries are the
premake projects `qjsx-engine` and `qjsx-engine-target`. Only the working folder on the disk
keeps the old name: `C:\Sandbox\espjs`.

## Decisions taken (do not reopen without the user)

- Engine: QuickJS by Bellard, version 2026-06-04 (`engine/qjs-bellard/VERSION`). Not quickjs-ng.
  mquickjs was evaluated and rejected (ES5 subset: no Promise/async, classes, modules...).
- Debugger client: VS Code through its built-in js-debug, so the protocol is CDP (not DAP).
- Debugger architecture: a minimal C stub inside the target exposing primitives (frames,
  slots, properties, loading and calling bytecode); the CDP adapter (proxy) is written in JS
  and runs on the development machine in a host QuickJS executable (not Node), which embeds
  the compiler: it compiles evaluated expressions and produces scripts' bytecode and debug
  info. quickjs-libc has no sockets and its Windows event loop only waits on stdin, timers
  and workers: the host needs C modules for an event loop, TCP sockets and the serial port.
  Desktop-only debugging is not a goal.
- Links between host and ESP: serial (UART, COM/USB), WiFi and wired network (TCP). The
  debug protocol must be transport-agnostic.
- Breakpoints/stepping: two dispatch tables (option C) chosen. Debug info (enriched pc2line
  with statement-start flags, function ids, variable scopes by pc) lives in a host-side
  "sidecar" cache indexed by a build id stored in the bytecode; the proxy checks source /
  bytecode / sidecar consistency. Evaluation: the proxy wraps the expression in a function
  whose parameters are the visible variables, compiles it on the host, the stub runs it.
- C++ <-> JS binding library (replaces the user's current uart/console code, which has bugs):
  C++17, header-only, NO std:: at all, no C++ exceptions, no RTTI. Agreed design:
  - errors: "first error wins" slot per call; `return js::fail(...)` (convertible to any return
    type), `js::fail<js::RangeError>()`, `js::failSystem()`; JS exceptions from callbacks are
    captured automatically; the boundary throws.
  - constraints carried by types: `js::Range<int,a,b>`, `JS_ENUM(Name, a, b)` (JS passes strings),
    `JS_STRUCT(Options, fields...)` for options objects (C++ defaults, unknown fields rejected).
  - own types: `js::Str`, `js::Bytes`, `js::Buffer`, `js::Function`, `js::Value`.
  - classes: `JS_CLASS(Name)` inside the class + `JS_EXPORT(member)` marker before each exposed
    method/field (`__COUNTER__` + overloaded static registrars, slots 0..63 scanned at compile
    time); JS constructor = optional `init(...)` method (C++ constructor has no arguments);
    fields become properties (read-only if const); `JS_MODULE(name, classes...)`.
  - destructor = safety net only (called by the finalizer, no JS allowed inside); explicit
    `close()`; `js::Value` members auto-register for GC marking (registered while the object is
    being constructed); `js::keepAlive(bool)` while an object has pending I/O (Node-like).
  - .d.ts typings for VS Code can be generated from the declarations.
- Event loop (all platforms, host and ESP): one JS thread; event producers (drivers, I/O
  threads, ISRs on the ESP) post messages into a single bounded queue (FreeRTOS queue on the ESP,
  mutex + condition/event on the host). The loop drains promise jobs, waits for the next
  message or timer, dispatches it on the JS thread. Messages carry a source id + generation,
  never raw object pointers. quickjs-libc's event loop is not used. The debugger stub's
  transport is one more producer; while paused, a mini-loop only handles debugger messages.
- JS libraries: `.qjslib` (agreed and IMPLEMENTED 2026-11-11; extension chosen by the user,
  consistent with `.qjspkg`; `doc/architecture.md` 4.5). Replaces the JS modules embedded in
  the firmware as source (`path`), which cannot load on a device (no compiler):
  - NAMES (user, 2026-11-11, on the model of `node:path`): every library module is named
    `<library>/<module>` and imported by that name: `import { join } from "stdlib/path"`,
    `import { read } from "sensors/bme280"`. The standard library is `stdlib`. Native modules
    keep their short names (`fs`, `crypto`). No short-name aliases;
  - a `.qjslib` is a zip laid out like a `.qjspkg` (manifest kind "library": name, version,
    compiler, natives, modules with their imports; `src/`, `debug/`, `bytecode/` as a cache);
    it always contains its sources (user: the FILE is what is delivered, not the source
    folder). `.d.ts`: not done;
  - `qjsx mklib [dir] [--output file]` makes one from a library project (`qjsx.json` with
    `name`, `version`, `modules`: module name -> source file); a public command, for the
    users' own libraries too (`host/js/library.js`);
  - the standard library: sources in `lib/stdlib/` (`path`, later the JS layers of the
    hardware modules); the build makes `stdlib.qjslib` next to the executable (premake
    post-build step running the tool just linked); the tool finds it from `process.argv[0]`,
    now the full path of the executable (`host/platform/executable_path.cpp`). The host
    executable ALSO embeds the same sources under the same names, for its own commands
    (`mklib` runs before `stdlib.qjslib` exists) and for `qjsx run script.js`;
  - other libraries are declared in `qjsx.json` (`"libraries"`: paths of `.qjslib` files);
  - `qjsx build` follows the static imports of every module of the program and copies into
    the `.qjspkg` every library module used, WHOLE (no function-level tree shaking),
    transitively; the bytecode of the `.qjslib` is used when the compiler id matches, else
    the module is recompiled from its source; the debugger sees them (sidecar and source in
    the package). A name without `/` that is no module of the program is a native module,
    listed in `natives` of the manifest;
  - the firmware contains no JS at all, only native modules; the device loader finds library
    modules in the package like any other. NOT DONE: `qjsx deploy` checking `natives`
    against a list the firmware would give in `Hello`;
  - dynamic `import()` is FORBIDDEN in programs built by `qjsx build` and in libraries (PC
    and devices alike), so the dependency graph is complete at build time: a compiler flag
    (`JS_EVAL_FLAG_NO_DYNAMIC_IMPORT`, engine change J) makes it a syntax error with its
    position. The tool's own JS (`qjsx:*`) is not concerned.
- Package of another engine (agreed and IMPLEMENTED 2026-11-11; `doc/architecture.md` 4.1,
  engine change K). The manifest of a package records `bytecode`, the bytecode version of the
  compiler (`JS_GetBytecodeVersion()` = BC_VERSION, 0x45 today; NOT the compiler id text, which
  changes with every qjsx version). Checked twice: the agent refuses `UPLOAD_BEGIN` when the
  number is not its own (nothing written, the installed package stays); the loader
  (`startPackage`) refuses to run such a package, on a device and with `qjsx run`. A package
  without the field is refused too. RULE: every engine change that alters the bytecode
  (opcodes, serialization) bumps BC_VERSION. A `.qjslib` records it too: a library made by
  another compiler is NOT refused (it holds its sources), its modules are compiled again and
  the build says so. Proposed, not done: a test that fails when the bytecode of a reference
  source changes (reminder to bump BC_VERSION).
- Compile warnings (agreed and IMPLEMENTED 2026-11-11, engine change K): `qjsx build` and
  `qjsx mklib` print `file:line:column: warning: ...` (in ENGLISH, user) for a direct
  `eval(...)`, `Function(...)` and `new Function`, which throw on a device; a warning, not an
  error (the build succeeds). Syntactic detection in the compiler
  (`JS_SetCompileWarningHandler`); indirect uses are not seen.
- Build package: one zip archive per build, extension `.qjspkg` (same extension for the full
  package and the reduced one sent to the ESP; the manifest says what it contains),
  entries stored without compression (no dependency; lets the ESP execute bytecode in place
  from flash via esp_partition_mmap + JS_READ_OBJ_ROM_DATA, to verify). Layout:
  `manifest.json` (format version, build id, compiler id, scripts: path, script build id, source
  hash), `bytecode/` (what goes to the ESP), `debug/` (one JSON sidecar per script), `src/`
  (exact copy of compiled sources). Sidecar positions are byte offsets in the source; the
  proxy derives CDP lines/UTF-16 columns. Script build id = hash of its bytecode with the id
  field zeroed. The proxy indexes the packages it finds to resolve the build ids reported by
  the stub.
- Stub <-> proxy protocol ("device agent": debug AND deployment):
  - framing on every link (serial and TCP): `0x00 | COBS(magic | version | CRC-16 | message) | 0x00`;
    on serial, bytes that do not decode as a valid frame are console text; during a session the
    stub wraps console/printf/ESP-IDF logs into `Log` messages.
  - encoding: hand-written CBOR subset (ints, floats, strings, bytes, arrays, integer-keyed maps).
  - objects stay on the target behind handles grouped like CDP object groups (`pause` released on
    resume, `console`, `persistent`); primitives travel inline; names never travel (sidecar).
  - requests are numbered (duplicates answered again, not re-executed); bad-CRC frames dropped;
    max message size negotiated in `Hello`; big payloads chunked (Upload Begin/Chunk/End).
  - messages: Hello; Pause/Resume/Step/SetStopPoints/SetPauseOnExceptions; ListScripts;
    GetStack/GetFrameSlots/GetProperties (getters never run implicitly)/GetInternals; SetSlot;
    LoadFunction + Call (js-debug helpers loaded once); Release; events Paused/Resumed/
    ScriptLoaded/Log/UncaughtException; plus deployment: write a `.qjspkg` to flash, list files,
    restart.
  - TCP: stub disabled by default, token required in `Hello`.
- Stepping: over = next statement start in the same or a calling frame; into = next statement
  start anywhere (native functions are stepped over, JS callbacks from C are entered); out =
  next statement start in a calling frame. Frames compared by ACTIVATION NUMBER (user,
  2026-11-11, replaces the C stack address and the stack depth): a new `JSStackFrame` field set
  to 0 when the frame is created (one store per call; async/generator states are zeroed by
  their memset), given a unique 64-bit number by `JS_DebugGetFrame` the first time it is asked
  (`JSDebugFrame.id`); an async/generator frame keeps its number across `await` / `yield`. At
  the start of a step the stub records the numbers of the frame and of all its callers; over
  stops in the frame or a recorded caller, out in a recorded caller. Engine change: microbench
  to redo. End of chain (user, 2026-11-11, as V8): when no recorded frame is on the stack any
  more, the step becomes an "into" and stops at the next statement run (next callback, often
  the awaiter of an async function), never in the start frame. An over from an ASYNC function
  (`JSDebugFrame.is_async`) records only that frame and waits for it (over `await` stays in
  it); a generator suspended by `yield` goes back to its recorded caller. A step started at a
  return position no longer counts its own frame. Known limit: an async function ending by an
  exception after an "over" from inside it leaves stepping on until the next pause.
  IMPLEMENTED 2026-11-11 (engine change L, which also fixed async/generator frames seen as
  native by core/debug.c); tested in Debug only, final test (Release, upstream tests,
  microbench, board) still to do. Stop at
  the start of a `catch` clause and at function end (each `return` + implicit return) to show
  the return value.
- Pause on exceptions: "all" pauses at the throw, before unwinding. "uncaught" predicts at the
  throw by scanning frames for catch offsets (offset 0 = iterator close and a handler starting
  with `gosub; throw` = finally rethrow do not count); needs a new `sf->call_sp` saved before
  each call (`cur_sp` already means something for generators). An exception crossing an async
  function boundary counts as caught; unhandled rejections are reported (console, stack captured
  at the throw) via the promise rejection tracker.
- Conditional breakpoints, logpoints and hit counts are compiled by the proxy and evaluated on
  the ESP itself (no round trip per hit).
- Proxy core without CDP (API agreed by the user 2026-11-11, IMPLEMENTED the same day in
  `host/js/debuginfo.js` and `host/js/debugsession.js`, tested in Debug only; the CDP server
  will sit on it unchanged). Small additions to the agreed API: `PackageIndex(paths, {
  sourceRoot })` (gives `localPath`), `breakpointAt` also returns `pcs` (every offset of the
  statement), `ScriptInfo.thisSlot(number)`, handler `onError`. Two embedded JS modules:
  - `qjsx:debuginfo` (no device): `PackageIndex(folders)` indexes `.qjspkg` by build id
    (manifest only); `DebugPackage` (`buildId`, `main`, `script(module)`, `scripts()`);
    `ScriptInfo` (`module`, `source` from `src/`, `library`, `localPath` or null for a library
    module, `check()` source/sidecar/local file consistency, `location(number, pc, { exact })`
    (level 0 exact: last position <= pc; callers: <= pc - 1), `breakpointAt(line, column)`
    (first statement position from there, innermost function; slides forward like V8),
    `statementStops()`, `functionName(number)`, `scopesAt(number, pc)` (local / block /
    closure, innermost first, internal variables hidden, `this` apart));
  - `qjsx:debugsession`: `DebugSession(device, index, { onPaused({ reason, frames, exception,
    returnValue }), onResumed, onScriptLoaded, onStarted, onExited })`; `attach()`,
    `setBreakpoint(module, line, column?)` -> `{ id, location }`, `removeBreakpoint`,
    `setPauseOnExceptions`, `pause/resume/stepInto/stepOver/stepOut`, `scopes(level)`,
    `properties(handle)`, `setVariable(level, name, value)`, `evaluate(expression, { level })`
    (wrapped in a function whose parameters are the visible variables, innermost wins;
    assignments do not reach the program: use setVariable), `detach()`. On SCRIPT_LOADED it
    sets the statement stops and the kept breakpoints then RESUME; on STARTED it reloads the
    package and re-applies breakpoints by line; frames resolved at each pause, variables on
    demand. Values decoded as `{ type, value }` / `{ type: "object", handle, kind, className,
    description, size, location }` (RemoteObject is the CDP step's job).
  - Lines and columns 0-based, columns in UTF-16 units, everywhere (CDP convention). Packages
    searched only in the project's output folder and in files given explicitly.
  - Tests: `tests/debuginfo.test.js` (no device: positions with accents and an emoji,
    breakpoint choice, scopes), `tests/proxy.test.js` (desktop agent, debuggee fixture, also
    runnable on the board).
  - Not in this step: conditions / logpoints / hit counts, global and script scopes,
    multi-statement evaluation, async stack traces.
- CDP server (design agreed by the user 2026-11-11, IMPLEMENTED the same day:
  `host/js/websocket.js`, `host/js/cdp.js`, `qjsx debug`, `doc/architecture.md` 3.9; tested
  in Debug by `tests/websocket.test.js` and `tests/cdp.test.js`; TRIED IN VS CODE by the
  user on 2026-11-11 with the desktop agent: "ça marche super"). Final test of 2026-11-11:
  Release and Debug, original QuickJS tests, firmware built and flashed, on the board
  debugger 72/72, proxy 30/30, cdp 50/50; microbench −3.3 % against the original (no
  regression from engine change L). Base64 turned out to be in the engine already (standard
  `Uint8Array.prototype.toBase64` / `Uint8Array.fromBase64`, `builtins/base64.c`): no
  module. SHA-1: `common/runtime/sha1.h` + `crypto.sha1`. On top of the proxy core.
  Facts read in js-debug's sources (main branch, 2026-11-11): attach with `"type": "node"`
  (not chrome: js-debug answers Target.* itself); it fetches `/json/version` and
  `/json/list` in parallel and takes `webSocketDebuggerUrl` (or `websocketAddress`); an
  error answer to a CDP command is almost never fatal; scripts announced with `file:///`
  URLs and no `hash`; breakpoints by `Debugger.setBreakpointByUrl` with a JS `urlRegex`
  matched against each script url, answer `breakpointId` + `locations`, later
  `Debugger.breakpointResolved`; destroying the default execution context closes its
  session. Decisions:
  - modules `qjsx:websocket` (HTTP `/json/version`, `/json/list`, `/json` + WebSocket:
    handshake, masked / fragmented frames, ping / pong, close) and `qjsx:cdp` (CDP methods
    -> DebugSession, session events -> CDP events); command `qjsx debug [--target t]
    [--port p] [--host 127.0.0.1]`, one VS Code client at a time;
  - SHA-1 in the C++ `crypto` module (common/runtime, next to sha256, devices too: "it will
    be useful"); base64 in C++ too (user, 2026-11-11), a native module for the programs as
    well (its name and API to propose before coding);
  - library module sources extracted from the package into `<output>/debug-src/` and
    announced as `file:///` URLs (a URL without scheme would be a Node internal, skipped);
  - a breakpoint with a condition, and a logpoint, are REFUSED until step 4 (VS Code shows
    them unbound);
  - the end of the program does not end the VS Code session (the default context is never
    destroyed): a redeploy or a device restart is picked up, breakpoints set again;
  - default port 9339 (not Node's 9229, which a Node debug session may use at the same
    time), settable in `qjsx.json`;
  - VS Code configuration: `node` attach, address 127.0.0.1, port 9339,
    `autoAttachChildProcesses: false`, `attachExistingChildren: false`, `skipFiles: []`,
    `sourceMaps: false`, `enableContentValidation: false`;
  - accepted and ignored for now: Profiler.enable, NodeWorker.enable,
    setAsyncCallStackDepth, setBlackboxPatterns, runIfWaitingForDebugger; no previews;
    `Runtime.callFunctionOn` compiled on the host, js-debug's helpers loaded once in the
    persistent group; program output as `Runtime.consoleAPICalled`;
  - tests: `tests/websocket.test.js` (SHA-1, base64, frames, handshake), `tests/cdp.test.js`
    (a JS WebSocket client replays js-debug's startup against `qjsx debug` + desktop agent),
    then a real try in VS Code by the user.
- Compiler changes (host compiler, "debug info" option when producing a `.qjspkg`). IMPLEMENTED
  2026-11-10, see `doc/engine-changes.md`; the two deviations below await the user's review:
  - statement / return positions: implemented as a kind in the 2 high bits of the
    `OP_line_num` operand (not a new opcode), emitted at each statement start, catch clause,
    loop test and update, each `return` + implicit return; resolve_labels records them at the
    next emitted instruction; peephole optimizations DO cross them (forbidding it cost 60 % on
    a microbench test: a marker skipped by a merge is emitted again or recorded,
    `js_debug_skipped_marker`, see `doc/engine-changes.md` E). Positions carry a kind
    (expression, statement, return).
  - DEVIATION: scopes are recorded as SOURCE ranges (push_scope / pop_scope), not pc ranges;
    the proxy maps the pause pc to a source position, then to the scopes containing it.
    Variables flagged lexical / const / captured; TDZ slots read as JS_TAG_UNINITIALIZED.
  - 16-bit function number per JSFunctionBytecode (creation order, serialized: bytecode format
    version bumped to 0x45). DEVIATION: no build id in the bytecode and no per-script table in
    the reader: the module name is kept in the bytecode, so a script is identified by package
    build id (manifest) + module name, a function by module name + number, found on demand by
    JS_DebugSetStops.
  - `debugger;` compiles to a new `OP_debugger` (no-op without debugger attached).
  - tail calls are ALWAYS disabled (same bytecode in debug and production). Note: QuickJS
    tail calls never dropped the caller frame; the separate OP_return is the return stop.
  - sidecar collected in js_create_function before stripping (JS_SetDebugInfoCollector); the
    host `compile(..., { debug: true })` returns `{ bytecode, debug }`, `qjsx build` writes it
    as `functions` in the sidecar JSON (format 2).
- Interpreter (implemented, `quickjs-debug.h`, `core/debug.c`): debug dispatch table chosen per
  frame, JS_SetDebugHook, stepping flag, breakpoint/statement bitmaps, frame and slot access,
  exception reporting once per thrown value. NOT DONE: caught/uncaught prediction
  (`sf->call_sp`).
- Host tool `qjsx`: a single executable (host QuickJS + C modules: event loop, sockets, serial,
  SHA-256, compile API; commands written in JS, precompiled and embedded). Commands: build,
  deploy, debug (proxy, CDP server for VS Code), monitor, targets, device ls|reset|info, trace,
  run. Targets: `serial:COM3`, `serial:/dev/ttyUSB0@921600`, `tcp:esp.local:5555`.
  Project file `qjsx.json` at the project root (name, main module, source folders + globs,
  default target, output folder, number of packages kept, `env` of the program on a device). VS Code: attach configuration
  (js-debug target type to verify: node vs chrome), scripts announced with `file:///` URLs of
  local sources, `.d.ts` of native modules written by `qjsx build`; breakpoints kept by the
  proxy and re-applied on ScriptLoaded and after an ESP restart.
  "Wait for debugger at boot" (`--wait-debugger`, remembered by the agent for the next boot,
  with a timeout, never on production devices): the agent holds `main` until the proxy says go,
  so startup code can be debugged (opening the serial port often resets ESP32 boards).
- Runtime library (replaces quickjs-libc, written with the binding library, shared with the ESP
  where it makes sense): globals `console`, `setTimeout`/`setInterval`/`clearTimeout`,
  `TextEncoder`/`TextDecoder`; `Uint8Array` for bytes (no Node `Buffer`). Modules: `fs` (sync),
  `stdlib/path` (JS, standard library), `process` (host), `net` (TCP, async), `serial` (host;
  `uart` on the ESP), `crypto` (`sha256`), `compiler` (host). Naming rule: when a function behaves exactly like Node's, use
  Node's name (hence `fs.readFileSync`, `path.join`, `process.argv`...); otherwise our own short
  names following the async rules (callbacks for events, promises for operations).
  System errors carry a Node-like `code` ("ENOENT").
- Uncaught JS exception in a callback on the ESP: print it and continue; restarting is an option.
- `process` on a device (agreed and IMPLEMENTED 2026-11-11, tested through the desktop agent
  by run-tests.sh; the desktop agent behaves like a target):
  - `exit(code)` ends the PROGRAM, never the process: `Loop::exit(code)` then an uncatchable
    exception (`JS_SetUncatchableException`, like an interrupt: no catch / finally runs); the
    loop drops it silently once exit was asked and runs no more job, timer or callback;
    `evaluateMain` accepts it. The runtime is freed, `runPackage` returns the code, the agent
    sends EXITED(code) and stays alive. No automatic restart (a program exiting at boot would
    loop); restart will be an explicit function. Same on the PC: `qjsx run` returns the code.
  - `argv` = `["qjsx", "<main module from the manifest>"]`, `env` = the strings of the
    optional `env` object of `qjsx.json` (validated, copied into the manifest by `qjsx
    build`, part of the build id), `{}` without one; never the PC's environment. Set by
    `startPackage` when `RunHooks::device` is true (the agent's runner). `qjsx run` of a
    package on the PC keeps the real argv and environment.
  - `cwd()` = `board::dataDirectory()` (desktop agent: absolute `<storage>/data`; a target:
    e.g. `/data`, a LittleFS partition), set by `runDevice` with `rt::setDataDirectory`; no
    `chdir`; `fs` resolves relative paths against it (`rt::resolveProgramPath`, fs's
    `ProgramPath`). A board without file system returns nullptr: cwd `"/"`, fs fails with
    ENOSYS. Whether `fs` exists on a target is still open.
- Long JS computation on a device (agreed, coded and verified on the YD board 2026-11-11):
  `platform::yieldIfDue()`, called by the runtime interrupt handler (`Runner::interrupt`),
  blocks the JS task for one tick every 100 ms (FreeRTOS ports; empty on the desktop), so the
  idle task of its core runs and the ESP-IDF task watchdog stays quiet. A JS endless loop is
  NOT seen by the watchdog, on purpose (undecidable; the device stays controllable from the PC
  through the same interrupt point: debugger pause, stop, deploy); only native code spinning
  is. Rejected: disabling the idle check of core 1, JS task at priority 0. Open: a time
  budget for a stuck script on a standalone device. See `doc/architecture.md` 5.5.
- Async styles in modules: callbacks for spontaneous events (`uart.onData(fn)`), promises for
  operations started by the script (`await uart.readLine({ timeout })`); a module may offer
  both. Async iteration (`for await`) maybe later, built on promises.
- No eval on the ESP at all, debug builds included: the compiler (`compiler/`) is not built
  for the ESP. Anything the debugger evaluates (`Runtime.evaluate`, `evaluateOnCallFrame`,
  `callFunctionOn`) must be compiled on the host and sent to the target as bytecode.
- On the ESP, JS code arrives precompiled from the development machine (no compilation of
  scripts on the target). Debug info can therefore live on the host, next to the proxy
  ("split debug info"), instead of in the target's bytecode.
- Breakpoint detection: two dispatch tables (normal / debug, the debug one checking a
  per-function bitmap of stop positions), with a per-opcode test as fallback when computed goto
  is unavailable. Opcode patching rejected (ROM bytecode, stepping). The ESP bytecode is
  stripped of positions, sources and variable names (same bytecode in debug and production):
  all debug info lives in the host-side sidecar (see below).

## Layout

Top level: `engine/` (QuickJS), `common/` (portable code built for the host AND the devices),
`host/` (the desktop tool only), `ports/` (device firmwares), `tests/`, `tools/`, `doc/`,
`legacy/`.

- `ports/cmake/qjsx.cmake` — what every device port builds (engine without compiler, common
  runtime and agent), sources / includes / defines / engine options (incl. GCC 14+ on 32 bits:
  `-Wno-incompatible-pointer-types`, `-Wno-format`, `-std=gnu11`).
- `ports/freertos/platform_freertos.cpp` — platform.h parts shared by the FreeRTOS ports
  (recursive Mutex, Signal = mutex + binary semaphore, sleepMs).
- `ports/esp32/` — ESP-IDF project, one firmware per BOARD (agreed 2026-11-11). FLASHED and
  TESTED on the YD board 2026-11-11: agent over UART0, deploy + run in place, debugger 52/52.
  `CMakeLists.txt` picks the board (`-D QJSX_BOARD=<folder of boards/>`, default
  `yd-esp32-s3-n16r8`), stacks `sdkconfig.defaults` (common: console UART0 921600 "custom"
  mode, no secondary console, 240 MHz) + `boards/<board>/sdkconfig.board` (chip, flash size,
  table from `partitions/`, PSRAM mode), sdkconfig per build folder `build-<board>/`.
  `partitions/4MB.csv` (no OTA), `8MB.csv` and `16MB.csv` (OTA slots reserved; OTA itself
  debatable). `boards/<board>/board_config.h`: data only (BOARD_NAME). `main/` shared by every
  ESP32 board: `main.cpp` (JS task 48 KB on core 1 → agent::runDevice), `board_esp32.cpp` (JS
  heap in PSRAM if CONFIG_SPIRAM else default; package slots = half of the "packages" partition
  each, header written last, esp_partition_mmap; UART0 link: driver + uart_vfs, one write per
  call, frames never cut), `platform_esp32.cpp` (esp_timer, esp_system_abort,
  xTaskCreatePinnedToCore). Headers are included by their bare name: the build puts every source folder
in the include path (no two headers share a name; keep it so).

- `engine/qjs-bellard/` — upstream QuickJS 2026-06-04 by Bellard, unmodified (reference, base of
  the engine diff).
- `engine/qjsx/` — the engine actually built (premake project `qjsx-engine`): QuickJS split into
  files (`core/`, `bytecode/`, `builtins/`, `compiler/`) by `tools/quickjs-split/regen.sh` from
  `engine/qjs-bellard/`, then MODIFIED BY HAND for the debugger (since 2026-11-10): every change
  is commented `qjsx:`, described in `doc/engine-changes.md`, exact diff in
  `doc/engine-changes.patch` (refresh it with `bash tools/quickjs-split/engine-diff.sh` after any
  engine change). `regen.sh` refuses to overwrite it without `--force` (ask the user before
  regenerating or editing it outside the debugger work). New files: `quickjs-debug.h` (public
  debugger API), `core/debug.c`, `compiler/debug-info.c`. `compiler/compiler.h` is private to
  `compiler/`.
- `tools/quickjs-split/` — the split generator, its config (line ranges of quickjs.c 2026-06-04),
  `regen.sh`, `engine-diff.sh`, engine test build and benchmark scripts. See its README.
- `common/jsbind/` — the C++ binding library (header only, implemented and tested).
- `common/runtime/` — event loop (`loop.*`), platform interface (`platform.h`), allocations
  (`memory.*`), UTF-8 file primitives (`files.*`), package reader (`package.*`, reads packages
  in place), SHA-256, modules (`modules/`: globals, fs, crypto; no JS: `path` moved to
  `lib/stdlib/`).
- `lib/stdlib/` — sources of the standard library (`qjsx.json` + `src/*.js`), compiled into
  `bin/<config>/stdlib.qjslib` by the build and embedded in the host executable as
  `stdlib/<name>`.
- `common/board/board.h` — what the common code needs from a board (identity, restart, JS
  allocator, package storage, packages read in place, links, console). One implementation per
  port: `host/platform/board_host.cpp` (desktop agent: files, TCP), `ports/...` (to come).
  Target-specific values come from the board, not from `#if` in the common code (user,
  2026-11-11): e.g. `board::platformName()` gives `process.platform` ("win32", "darwin",
  "linux" on the desktop; "esp32" and "rp2350" on the targets, user's choice).
- `common/runtime/program.{h,cpp}` — runs a program or a package in a new runtime (runtime,
  context, loop, module loader: native, package bytecode, embedded JS, then a hook; host
  adds the files on disk in `host/host.cpp`). `common/agent/runner.{h,cpp}` —
  `agent::runDevice()`: agent + debugger stub + runner of the installed package, the same on
  every board (`qjsx agent` = `host/agent_command.cpp` + the desktop board).
- `common/agent/` — the device agent, portable C++ (protocol: cbor, frames, protocol.h which must
  match host/js/protocol.js; agent core with Link / Storage / Controller interfaces) and the
  debugger stub (`debugger.{h,cpp}`, `debugger_values.cpp`: on the JS thread, requests queued
  from the transport thread; tested by `tests/debugger.test.js` through the desktop agent,
  fixture `tests/fixtures/debuggee`). Host side: the proxy core (`qjsx:debuginfo`,
  `qjsx:debugsession`) is written; the CDP server is not.
- `host/` — the `qjsx` executable: `main.cpp`, `host.*` (runtime + module loader, `RunHooks`),
  `agent_command.cpp` (`qjsx agent`, desktop stand-in for an ESP), `platform/platform_host.cpp`,
  `modules/` (host-only native modules: net + sockets, serial, compiler), `js/*.js` = the tool
  commands written in JS, embedded as modules `qjsx:<name>` by a premake build rule
  (`tools/embed-js.lua`).
- Packages are compiled fully stripped (`strip: "all"`): the engine keeps the module name
  outside the debug info, so import.meta / dynamic import still work.
- `tests/` — `run-tests.sh`, `assert.js`, `jsbind.test.js` (binding library, through
  `tests/native/test_module.cpp`, built only into `qjsx-tests`), `debug.test.js` (engine debugger
  API, through `tests/native/debug_module.cpp` and `tests/fixtures/debug/target.js`),
  `debugger.test.js` (stub), `debuginfo.test.js` and `proxy.test.js` (proxy core),
  `websocket.test.js` and `cdp.test.js` (CDP server),
  `runtime.test.js`, `tools.test.js`, `protocol.test.js`,
  `net.test.js`, `fixtures/`.
- `doc/architecture.md` — full design + status of each part (keep it updated);
  `doc/engine-changes.md` (+ `.patch`); `doc/quickjs-map.md` (map of quickjs.c, description of
  the split; `doc/quickjs-items.csv`: every item); `doc/quickjs-split-perf.md` (original vs
  split vs qjsx: tests, sizes, compile times, microbench).
  Vocabulary (user, 2026-11-11): the docs say "device" (le device) for the target boards,
  defined at the top of architecture.md: today ESP32 and RP2350, a list that will grow. A chip
  is named only for what is specific to it (ESP-IDF, esp_partition_mmap, RP2350 boot ROM...).
- `TODO.md` — user's backlog; its first section "Où on en est" is the state of the work in progress:
  read it first when resuming. `doc/known-bugs.md`: defects found and not fixed yet; once fixed, a defect moves to
  `doc/history.md` with what was done (user, 2026-11-11).
- `legacy/` — the user's old program (`main.cpp`, `utils.h`, `devices/`, old `Makefile`,
  `qjs.make`, `qjs.exe`, `boot.js`, `test.js`, `test.zip`), no longer built (the user agreed to
  rewrite it; `devices/console.cpp` never compiled). Not under git: only the user deletes it.

## Build

- Toolchain: Windows, gcc 13.1 mingw64, premake5 5.0.0, node 26. Makefiles are generated with
  `premake5 gmake --cc=gcc --shell=posix` (the user's "unix makefiles").
- Project build: `premake5 gmake --cc=gcc --shell=posix` then `make -C build config=release`
  (or `config=debug`). Projects: `qjsx-engine` (engine/qjsx), `qjsx-engine-target`
  (the same without compiler/, QJSX_NO_COMPILER: no eval, like the targets), `qjsx` (the
  tool), `qjsx-tests`, `qjsx-target` (qjsx without its compiler module, linked with
  qjsx-engine-target: runs packages like a
  target; checked by run-tests.sh with tests/fixtures/nocompiler). JS modules embedded as
  source (`qjsx:*`, `stdlib/*`) only work on the host; a target gets library modules as
  bytecode in the package (`.qjslib`).
  Executables in `bin/Release/` and `bin/Debug/`.
- Tests: `bash tests/run-tests.sh Release` (and `Debug`: JS_FreeRuntime asserts that no JS
  object leaked). It runs jsbind/runtime/tools tests, builds `tests/fixtures/demo` and checks
  that the package runs exactly like the sources. Always use `timeout` when running scripts by
  hand: a hang means the loop waits forever.
- ESP-IDF v6.1 (ports/esp32): idf.py only works inside the ESP-IDF PowerShell environment,
  never from the PowerShell or Bash tools. Claude runs it in a new Terminal panel tab with
  run_in_terminal: `powershell.exe -NoProfile -ExecutionPolicy Bypass -File
  C:\Sandbox\espjs\tools\idf.ps1 build` (or `... ninja -k 0` for every error at once); the
  script loads the user's IDF profile, logs to C:\temp\idf-build.log (UTF-16: iconv before
  grep) and writes the exit code to C:\temp\idf-status.txt last (wait for that file in a
  background Bash loop rather than reading the terminal, whose output is huge). Flashing
  (`idf.py -p COMx flash monitor`) acts on the board: ask the user first. Test board:
  VCC-GND YD-ESP32-S3, WROOM-1 **N16R8** (16 MB flash, 8 MB OCTAL PSRAM in the chip package, as
  `esptool flash-id` reports; the user first read N4R2), wired by its **COM** port (CH343 →
  UART0), COM11 on the user's PC. `tools/idf.ps1 [board=NAME] build|-p COM11 flash|ninja
  -k 0|esptool ...` builds in ports/esp32/build-NAME.
- Second target board: Olimex PICO2-XXL (RP2350B, 48 GPIO, 16 MB QSPI flash, 8 MB QSPI PSRAM on
  QMI chip select 1 = GPIO8, one USB-C, microSD, UEXT, BOOT + RESET buttons; schematic Rev. C in
  github.com/OLIMEX/RP2350-PICO2-XXL). Multi-platform plan AGREED 2026-11-11:
  ports/{cmake,freertos,esp32,rp2350}, a board.h interface (reset, identity, memory / PSRAM
  allocator, package storage, links), platform.h completed for an RTOS, one
  platform_freertos.cpp for both targets; order: engine without compiler validated on the PC,
  then ESP32-S3, then RP2350. The board.h / platform.h APIs are proposed to the user first.
- After an engine change, also run the upstream engine tests and microbench
  (`tools/quickjs-split/test`, README there; build dir e.g. `C:\temp\qjs-engine-test`, upstream
  release extracted in `C:\temp\qjs-update\v2026-06-04\quickjs-2026-06-04`). Rerun premake
  there when engine files are added. Never benchmark while something else is building.
  If that build fails with "No rule to make target ... src/quickjs/...", its makefiles and
  dependency files are older than the folder layout: rerun premake there and delete its
  `obj/` folder (done 2026-11-11). `run.sh` runs the tests on whatever binaries are there:
  check that `make` succeeded first.
- After adding or removing a source file of `common/` or `ports/`, the ESP-IDF build needs a
  CMake pass (ninja keeps the old file list): `tools/idf.ps1 reconfigure build`.
- Pitfalls met: `JS_NewTypedArray` reads offset/length even when argc is 1 (always pass 3
  arguments); `JS_ThrowPlainError` and `JS_NewModuleValue` do not exist in this QuickJS; on
  32-bit (ESP) JSValue is a plain uint64_t (never specialize templates on JSValue); members
  declared by JS_CLASS are private (go through ClassAccess); C++ names like `log`/`exit`
  clash with libc (use JS_FUNCTION_AS); in a void function write `return (void)js::fail(...)`.
- Engine tests and benchmark: `tools/quickjs-split/test/` (needs an extracted upstream release,
  https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz, for qjs.c, quickjs-libc.c, repl.js, tests/).

## Coding rules

The full rules, with examples, are in `doc/coding-rules.md` (written to be reused in other
C / C++ projects). Summary of what was agreed:

- File header (user, 2026-11-11, APPLIED to 134 files the same day): every source file of
  the project starts with the block of `doc/coding-rules.md` 3 ("File header"): "qjsx" in
  ASCII art, "qjsx - JavaScript for devices", "Author: Etienne Cochard", "Copyright (c) 2026
  R-LIBRE INGENIERIE" (exactly as in LICENSE), "SPDX-License-Identifier: MIT (see LICENSE)".
  A `/* */` block in C / C++ / JS (a `//` line ending with `\` would continue the comment);
  `# ` lines in shell / CMake / Perl / PowerShell / sdkconfig / partition CSV (after `#!`),
  `-- ` in Lua. Never in `engine/qjs-bellard/` nor in the QuickJS files of `engine/qjsx/`
  (Bellard's notice stays; only quickjs-debug.h, core/debug.c, compiler/debug-info.c have
  it), nor in JSON / Markdown. A new file gets it too. The fixtures that tests check by line
  number moved by 7 lines (run-tests.sh updated).

- Parameters (agreed with the user 2026-11-11): never a non-const reference in our APIs, and no
  const reference either: an input struct goes by value if small (copied, cannot change) or as
  `const T *`; an output goes through an explicit pointer `T *out` that the function fills, so
  that `&variable` at the call shows it changes. Applies to all our code except the internals
  of `common/jsbind` (generic templates keep their `const T &`, the user-facing API is the same).
- Casts (user, 2026-11-11): C casts `(T)value` instead of `static_cast<T>(value)`; parenthesize
  when a member follows: `((Runner *)opaque)->run()` (`(T *)p->m` casts the member, not p).
- Private members (user, 2026-11-11) start with an underscore: `_state`, not `state_`.
  Exception: name prefixes pasted by macros (`jsNewModule_##name`...).
- Layout (user, 2026-11-11): readability and maintainability first. A function's opening
  brace is on its signature line (`void run() {`). Doc comments are `/** ... */` blocks before
  functions: nothing for the obvious, but describe the flow (numbered steps, threads, order of
  operations, why). In function bodies, blank lines separate the logical groups (after a
  closed block, before a loop, between steps). In headers, the names of the members and
  methods of a class are aligned in one column (a `*` or `&` hangs before the column).
- No code on one line (user, 2026-11-11): braces always, and a body always on its own lines:
  `if (x) {` / `    print();` / `}`, never `if (x) print();` nor `if (x) { print(); }`; the same
  for loops and for function bodies (`int size() const {` / `    return _size;` / `}`).
- Members of structs and classes (user, 2026-11-11): a short `//` comment at the end of the
  line (what it holds, unit, who owns it, which thread or lock), the comments of consecutive
  members aligned in a column; a `//` line above when it does not fit. Checked by
  `tools/undocumented-members.pl`.
- Section separators (user, 2026-11-11): `// ### Title ####...` filled with `#` up to column 80,
  or a bare `// ####...` line; never `// --- title ---` nor a `/** */` block.
- Every `case` of a switch has its block between braces (user, 2026-11-11): `case X: {` / statements
  / `break;` / `}`; labels that fall through to the next one stay bare. The cases are indented
  one level under their `switch`.
- Stack (user, 2026-11-11): thread stacks are small on an RTOS (a few KB): no large array on
  the stack (more than about 256 bytes). I/O threads read straight into the malloc'd block
  they post to the loop (`sockets::receiveBlock`, serial `readLoop`), or into a buffer
  allocated once on the heap.
- Paths (user, 2026-11-11): on Windows every path the runtime handles or gives to JS uses
  '/' (Windows accepts it), through `files::useSlashes` (C++) and `toSlashes` (module path);
  `\\?\` (long) and `\\.\` (device: COM10...) paths keep their '\' untouched. On POSIX '\' is
  an ordinary name character, never converted.
- Allocations (user, 2026-11-11): our code allocates only through `memory::allocate` /
  `reallocate` / `release` / `copyText` (`common/runtime/memory.h`), never malloc / realloc /
  free directly; out of memory is fatal (`platform::fatal`: abort on the desktop, restart on a
  target), so the results are never tested against nullptr. Single entry point, so
  allocations can be counted or traced (profiling). The QuickJS heap (js_malloc) is apart.
- Every function and method has a `/** */` comment (user, 2026-11-11): one or two lines saying
  WHAT it does (in headers also ownership, thread, error return). The flow (HOW) goes INSIDE
  the body as pseudo-code: a short `//` comment above EACH group of statements, even an
  obvious one (user, 2026-11-11: "agent.cpp est super tel qu'il est"; reference:
  `common/agent/agent.cpp`). The `/** */` describes parameters and the return value ONLY
  when they are not obvious (user, 2026-11-11): no systematic @param / @return list. Blank lines cut a body into its steps: build,
  convert, open, check, loop, close... each step is a group (a check stays with the step it
  checks; a blank line follows a closed block, `} while (...);` included).
  `tools/style.pl` applies the mechanical part (braces, `/** */` above
  functions, alignment, blank lines after blocks / before loops, casts); the logical groups
  and the flow documentation are done by hand.

## Working notes

- SUBAGENTS (user, 2026-11-11): every subagent is launched with the model Opus 5.5
  (`model: "opus"`) and a high reasoning effort (`effort: "high"`), never with the model of
  the session (Fable 5.2 costs too much for delegated work: five agents were launched that
  way on 2026-11-11 without warning, about 460,000 tokens for four translations). Say in one
  line what is launched, and why, before launching.

- Git (2026-11-11): the repository is https://github.com/rlibre/qjsx.git (remote `origin`,
  branch `main`, NOT `master`: before giving a push command, check the name of the local
  branch against the default branch of the remote). `.gitignore` leaves out the build outputs
  (`build/`, `bin/`, `ports/esp32/build-*/`, any project's `build/`, `*.qjspkg`, `*.qjslib`),
  `legacy/` and `doc/prompt-reprise.md` (kept on disk, not published: user);
  `.gitattributes` keeps LF line endings everywhere (shell scripts, and the
  source hashes of the packages must not depend on the machine). The user commits and pushes:
  git stays read-only for Claude unless the user asks for a precise command.

- LANGUAGE (user, insisted 2026-11-11): EVERYTHING the user can see is in French, tutoiement:
  messages, questions and their choices (AskUserQuestion), task list items, wrap-up recaps and
  questions, the `description` of every command (Bash, PowerShell), terminal tab titles, agent
  briefs' summaries relayed to the user. Code, comments and these notes stay in English.
- DOCUMENTATION IN ENGLISH (user, 2026-11-11): `README.md` and every file of `doc/` are
  written in English (they were translated that day), like the messages the tool prints.
  `TODO.md` (the user's backlog) and `doc/prompt-reprise.md` (personal, not in git) stay in
  French. Only the conversation with the user is in French.
- quickjs.c is ~61k lines: search it with Grep and read ranges; `doc/quickjs-map.md` gives the
  line ranges of each module.
- Interpreter: `JS_CallInternal` in `engine/qjsx/core/interpreter.c` (computed-goto
  dispatch, `DIRECT_DISPATCH`); frames: `JSStackFrame`, chained from `rt->current_stack_frame`.
