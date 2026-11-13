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
    are named with the reserved prefixes `sys/` and `device/` (`sys/fs`, `device/gpio`: see
    "Names of the native modules", DONE 2026-11-11). No short-name aliases;
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
    the package). A `sys/...` or `device/...` name is a native module, listed in `natives` of
    the manifest; a bare name (`fs`) is an error;
  - the firmware contains no JS at all, only native modules; the device loader finds library
    modules in the package like any other. DONE 2026-11-11: the agent lists the native
    modules of its firmware in the HELLO answer (`NATIVES`, from rt::countNativeModules /
    nativeModuleNameAt) and `qjsx deploy` refuses a program that imports another one
    (`upload()`, host/js/deploy.js); an older firmware is not checked;
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
  package and the reduced one sent to the ESP; the manifest says what it contains; DONE
  2026-11-11: `upload()` sends `manifest.json` with `"reduced": true` and its scripts without
  `debug` / `source`, and `bytecode/` only; same build id),
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
- Pause on exceptions: "all" pauses at the throw, before unwinding. "uncaught" (user,
  2026-11-11, replaces the `sf->call_sp` design, IMPLEMENTED the same day, engine change M): the engine predicts at the throw,
  only when a hook is set (no cost on the hot path): in the throwing frame by scanning its
  real stack for catch offsets; in each calling frame STATICALLY, by checking in its bytecode
  whether its pc lies in a try region (from an `OP_catch` to its handler); offset 0 = iterator
  close and a `gosub; throw` / plain `throw` handler (finally rethrow,
  js_debug_is_rethrow_handler) do not count; an async function frame counts as caught (the
  exception becomes a rejection); reaching the bottom of the stack = uncaught. Result in a new
  `JSDebugEvent.uncaught`; the stub pauses in "uncaught" mode only when it is true. Known
  approximation: a throw in a promise callback (`then`) is seen as uncaught. Unhandled
  rejections reported via the promise rejection tracker: not done.
- Debugger step 4 (user, 2026-11-11, IMPLEMENTED the same day; tested in Debug on the desktop agent and on the ESP32-S3: debugger 72/72, proxy 30/30, cdp 53/53), besides the "uncaught" prediction above:
  - `qjsx debug --restart` (opt-in, off by default): on `Runtime.runIfWaitingForDebugger`
    (js-debug sends it once its breakpoints are set) the server sends RUN, so the program
    starts again with the debugger attached and each module waits at SCRIPT_LOADED for its
    breakpoints: startup code can be debugged, and VS Code's Restart restarts the program.
    Only the program restarts, not the chip. REPLACES the persistent `--wait-debugger` flag
    (dropped for now; to reconsider only to debug what follows a real chip reset);
  - `this` of the frames: read at the pause for the frames whose function has a `this`
    slot (ScriptInfo.thisSlot); elsewhere `undefined`, described as not available;
  - object previews (CDP `preview`) built on the host when js-debug asks for them: one more
    GET_PROPERTIES per object, at most 5 properties, nested values by their description;
    to measure on the board (next step if too slow: a small preview added by the stub).
  - Conditional breakpoints, logpoints, hit counts: postponed (TODO backlog).
- Package in RAM and restart after deploy (agreed by the user 2026-11-12, IMPLEMENTED the same
  day, Debug suite 66 OK; TRIED on the ESP32-S3 2026-11-12: deploy --ram, device info, reset
  drops it, deploy --reset, then debugger 72/72 (one run 71/72 just after a deploy --reset,
  not reproduced in 3 runs), proxy 30/30, cdp 53/53; RP2350 and STM32 not now, user):
  `UPLOAD_BEGIN` with `VOLATILE` (key 44)
  loads the package in RAM (`common/agent/ram_storage.*`, exact size from the C heap via
  `memory::tryAllocate`, the only allocation allowed to fail; refused with "not enough RAM"),
  nothing written to flash; RUN starts it instead of the installed one; lost at a chip restart
  (the desktop agent's RESET drops it); a flash install drops it; INFO reports `VOLATILE`.
  Deviation from the proposal: RAM comes from the C heap on every board (not PSRAM per
  board). Tool: `qjsx deploy --ram` (warning printed), `qjsx debug` uploads the project's
  latest package in RAM by default (`--no-upload` to skip; RUN, or STOP with --restart);
  `qjsx deploy --reset` (flash only, refused with --ram): RESET then `Device.reopen` (retries
  until HELLO answers, 20 s), the program starts at boot.
- Digital twin of a board (design agreed by the user 2026-11-12, IMPLEMENTED the same day for
  the YD-ESP32-S3, `doc/architecture.md` 6.6; Debug suite 74 OK, `tests/twin.test.js` 17
  checks, page tried in the browser pane): `qjsx agent --board <name | file.json>
  [--ui-port 8090]`; board JSON in `boards/` (copied to `bin/<config>/boards/` by the
  build); `host/twin/sim/twin.cpp` (hal::gpio: output latch wins, then open drain at 0, then what
  the page forces, then pulls incl. the board's for an active-low button; change handler
  called at once on the changing thread), `twin_rmt.cpp` (hal::rmt: send lasts its ticks,
  done on a "twin rmt" thread, bytes kept per pin), `twin_module.cpp` (native
  `qjsx:twinboard`, registered only in a twin); only the board's natives offered to the
  program (`rt::setDeviceNativeModules`), its `process.platform`, agent name "qjsx twin";
  page server `host/twin/js/server.js` (`qjsx:twin/server`) run by the hidden command
  `qjsx twin-ui` in a second JS runtime on its own thread (4 MB stack); it must print
  nothing (one output sink per process). No executable involved (user asked, 2026-11-12):
  the tool plays the board. PAGE IN X4JS (user, 2026-11-12, DONE the same day, Debug suite
  88 OK, tried in the browser pane): `host/twin/ui/` (x4js project, `npm install` done with
  the user's agreement; `node_modules/` git-ignored; `dist/` KEPT in git with
  `dist/sources.crc32`); messages x4js `{ msg, params }` on WebSocket `/ws`, subprotocol
  `messaging`; embedded as a stored zip by `tools/embed-twin-ui.lua` (runs `npm run build`
  when the sources changed and node_modules exists, else warns); hidden option
  `qjsx agent --ui-dir <folder>` to serve the page from disk. Page (user, 2026-11-12): header
  (microprocessor logo SVG, title, board, link state, settings button), settings in a
  `SettingsView` page in a `Dialog` (theme, used pins only; kept by the browser with
  `loadPState`), panels shown / hidden by a CONTEXT MENU, layout kept (`persist`); ONE font
  size, 12px (user: `--font-size` of x4 set on :root; no other font-size); x4js is READ-ONLY for us (`node_modules`; the user
  fixes x4 himself and publishes it on npm); types: `tsc --noEmit -p host/twin/ui` (tsc is
  on the machine). Everything of the twin lives in
  `host/twin/` (user): `sim/` (C++), `js/` (embedded as `qjsx:twin/<name>`), `ui/`, later
  `parts/`; `qjsx:twin` = the PUBLIC API of the component models (design in TODO.md).
  PARTS (agreed and IMPLEMENTED 2026-11-12, `doc/architecture.md` 6.6, Debug suite 94 OK + 1
  fixed and run again alone, `tests/twinparts.test.js` 21/21): a part = a model (JS, in the twin's runtime, never the
  program's) + a view; `host/twin/api/twin.js` = `qjsx:twin` (`Part`, `Registers`,
  `board.pin/i2c/rmt/now/supply`, `pin.voltage`); parts of qjsx in
  `host/twin/parts/<model>/model.js` = `qjsx:twin/parts/<model>` (button, ws2812, bme280,
  potentiometer); a project's part = `"model": "./folder"` (model.js, optional view.js);
  `host/twin/js/parts.js` builds them (ids unique; first error: the agent prints it and
  exits, `twinboard.ready`); `host/twin/sim/twin_bridge.cpp` posts the hardware events to
  the twin's runtime (pins watched by parts, RMT bytes / pulses, I2C transfers waited
  synchronously by the program's thread, timeout = the transfer's; the twin never waits
  for the program); pin priority: program output > open drain at 0 > page force > part
  drive > pulls (program's, then part's), `conflict` flag. Boards: `kind` -> `model` + `id`
  (`boot` button, `rgb` ws2812), `"adc"` section. Project: `qjsx agent` in the folder (or
  `-p / --project`) reads `"twin"` of `qjsx.json` (`board`, `parts`, optional `ports` {
  agent, ui, usb, uart }); `"target": "twin"` = tcp:127.0.0.1:<ports.agent | 5555>. Page:
  three levels of views (state list; `static controls` slider / knob / switch / select /
  button / value / led; `createView(x4, part)` of a project's view.js), section "Buttons",
  part titles "label · model" with the live use of their pins, I2C sections only for an
  open bus with no part (trace of the transfers, frequencies), console of the parts.
  User's UI rules: show the page (with its URL) before running the tests; one font size;
  knobs of a fixed width; no exaggerated effects.
- File access of a PC program (agreed with the user 2026-11-12, IMPLEMENTED the same day,
  `doc/architecture.md` 6.7, Debug suite 86 OK; a DEVICE HAS NO PERMISSIONS, user: the runner
  only sets its working folder, EXCEPT the desktop agent and the twin (same runner code, on
  the PC's disk): `board::confineFiles()` (true only in board_host.cpp) limits their files to
  `<storage>/data` (user, 2026-11-12; net / serial / env stay free); `rt::Permissions` in common/runtime/permissions.*, built by
  host/permissions_host.cpp, checked in fs / net / serial / process.env). Before it, the
  discussion: today
  `sys/fs` is unrestricted on the PC (exe, `qjsx run`) and only relative paths are rooted in
  `<storage>/data` on the desktop agent / twin (absolute paths and `..` escape). Goal (user):
  a JS bug must not be able to wipe a disk. Agreed so far, on Deno's model: a `"desktop"`
  section in `qjsx.json` (copied into the manifest) with `"rootFolder"` and `"permissions": {
  read, write, net... }`; NOTHING allowed by default; the program running as a device (board,
  agent, twin) keeps the board's rules (its data folder), not `desktop`. Variables (names
  chosen by the user): `${executableFolder}`, `${userAppDataFolder}` (`%APPDATA%/<name>`,
  `~/Library/Application Support/<name>`, `$XDG_DATA_HOME` or `~/.local/share/<name>`),
  `${currentFolder}` (only when written), `${rootFolder}`. Must work on Windows, macOS and
  Linux. Also agreed (user, 2026-11-12): `rootFolder` declared without `permissions` = read
  allowed in the root, write refused; a root at a file system root (`/`, `C:/`) is refused;
  `qjsx run script.js` gets nothing except what its command line allows (Deno-like
  options); `${userAppDataFolder}` is NOT created at launch; `process.cwd()` =
  `${rootFolder}` for a PC program, as on a board (relative fs paths resolve there); without
  `rootFolder`, `cwd()` is the real current folder (no file access anyway). The root is
  overridden at launch by a reserved command line option (removed from `process.argv`;
  user prefers it: two projects at the same time) `--qjsx-root-folder=<folder>` AND by the
  environment variable `QJSX_ROOT_FOLDER`; precedence: option > variable > `qjsx.json`.
  `${executableFolder}` under `qjsx run app.qjspkg` = the package's folder. `qjsx run`
  options as Deno's: `--allow-read[=paths]` `-R`, `--allow-write[=paths]` `-W`,
  `--allow-net[=hosts]` `-N`, `--allow-env` `-E`, `-A`, plus `--allow-serial[=ports]` and
  `"serial"` in `permissions` (opening a port acts on hardware). `--qjsx-root-folder` is the
  ONLY runtime option of an executable (its permissions are baked in; the prefix avoids
  stealing a program's own options). `qjsx run` options take no prefix: before the script
  name they are qjsx's, after it the program's (as Deno). Paths compared as real
  paths (links resolved); a recursive rmSync never follows a link (removes the link, not
  its target): `removeTree` already used lstat. Tests: never a destructive probe outside
  the test's own folder (user); Git Bash turns a "/" argument into its own folder, so the
  disk root test uses "C:/" on Windows and a script granted nothing.
- Persistent storage on a device (design agreed by the user 2026-11-13, NOT implemented):
  - `sys/fs` unchanged, on LittleFS in the `data` partition mounted on `/data`
    (`board::dataDirectory()`; fs goes through fopen / stat, so the ESP-IDF VFS is enough);
    component `joltwallet/littlefs` (version pinned in `idf_component.yml`, download at
    implementation time); formatted automatically when the mount fails. LittleFS commits a
    file at its close: a power cut keeps the old content whole. No migration of the NVS
    of a client's former C firmware (user);
  - `fs` CONFINED to `/data` on the boards too (`confineFiles()` true, as the desktop agent
    and the twin): the ESP32 VFS also holds `/dev/uart/0`, the agent's link. A program
    cannot read or change its own code: packages and firmware are raw partitions, no
    compiler on the device, `import()` refused;
  - tool commands (names chosen by the user): `qjsx device ls [folder]`, `read <file>
    [local]` (no local: printed), `write <local> [file]`, `rm <file | empty folder>`,
    `mkdir <folder>`, `mv <from> <to>` (replaces an existing file, as Node's renameSync;
    a folder never replaces a file nor the reverse); paths relative to `/data`, `..` and paths outside
    refused by the agent; `qjsx device info` adds used / total of `/data`;
  - agent messages on every link: FILE_LIST, FILE_READ (chunks bounded by the message size
    of HELLO), FILE_WRITE_BEGIN / CHUNK / END (temporary file renamed at END: atomic),
    FILE_REMOVE, FILE_MKDIR, FILE_RENAME; Node-like error codes; allowed while the
    program runs; same on the desktop agent and the twin (`<storage>/data`);
  - project data folder: `"data"` in `qjsx.json` (e.g. `"data/"`), sent by `qjsx deploy
    --data`: every file of the folder written, same names overwritten, the other files of
    `/data` kept (what the program wrote itself stays);
  - SD card later: FatFS on `/sd` (readable on a PC; LittleFS is slow on large volumes),
    confinement extended to the list of mount points; on RP2350 / STM32 (no VFS) one small
    layer of ours routes paths to LittleFS or FatFS.
- `device/board` (agreed by the user 2026-11-13, NOT implemented; name chosen by the user
  among device/board, device/system, device/identity, sys/os): what belongs to the board
  the program runs on:
  - `board.id()` -> `Uint8Array`, the unique id of the chip (ESP32: base MAC from the
    eFuses, 6 bytes; RP2350: unique id of the flash, 8 bytes; STM32: UID, 12 bytes); the
    program formats it itself;
  - `board.mac(kind)` (ESP32): `"base" | "wifi" | "ap" | "bluetooth" | "ethernet"` ->
    `Uint8Array` of 6 bytes (ESP-IDF derives them from the base MAC);
  - `board.restart()`: restarts the chip (exact behaviour to agree: what is flushed first);
  - twin and desktop agent: the id / base MAC is kept in `<storage>`; when missing, drawn
    at random once and saved (same installation = same id);
  - later, with WiFi: `networkInterfaces()` as Node's (with `mac`) in `sys/os`;
  - random bytes: the web standard `crypto.getRandomValues(typedArray)` (global, as in
    Node and browsers), agreed in principle; source to design (ESP-IDF: the hardware RNG
    is truly random only with RF on or the SAR ADC entropy source enabled: to check).
- JS developer manual and typings (user, 2026-11-13, IMPLEMENTED the same day):
  - `.d.ts` WRITTEN BY HAND (user chose this over generation from jsbind: the API still
    moves, callbacks / js::Value need hand types anyway), one NEXT TO EACH MODULE (see
    "TYPINGS" in the coding rules), `@module <name>` in the comment before `declare
    module` (TypeDoc names the module by it); written by three agents from the code,
    then checked by tsc;
  - embedded in `qjsx` as `qjsx:types` (`tools/embed-types.lua`, TYPING_FILES; premake
    rule on `common/runtime/modules/globals.d.ts`);
  - `qjsx build` (cli.js, `host/js/typings.js` = `qjsx:typings`) writes them FIRST (even
    when the build fails) to `<project>/.qjsx/types/index.d.ts` (NOT node_modules/@types:
    npm's name space, and npm install removes it, user) and, when the project has no
    jsconfig.json / tsconfig.json, a jsconfig.json with `"types": ["./.qjsx/types"]`
    (needed: TypeScript 6+ loads no types package by itself; an entry of `types` is
    loaded whatever `include`), `lib: ["esnext"]` (no DOM). An existing configuration is
    NEVER changed: a FRAMED warning (user: "bien visible") gives the line to add; a
    failure to write them is a framed warning too, never a failed build. Checked with tsc
    on a project: real errors found ("open-drain" -> did you mean "openDrain"?);
  - DRIFT TEST `tests/types.test.js` (run by qjsx-tests, which also registers the
    device/* modules for it): every sys/ device/ stdlib module has a typing and the
    reverse; exports, prototype and static names of each class (a JS class of the stdlib
    may set declared properties per instance), members of the global namespaces (own
    names: process / console properties are not enumerable), non-standard globals;
  - manual: `doc/jsmanual/` (Astro 7 + Starlight 0.42 + starlight-typedoc, versions pinned
    in package.json, node_modules / dist / .astro git-ignored): guides written by hand
    in `src/content/docs/guides/`, API reference generated from the typings
    (`tsconfig.typedoc.json` and the list in `astro.config.mjs`: add a new module to both;
    `api-readme.md` = its introduction, needed or the module pages are dropped);
    `npm run build` / `npm run dev` there (`.claude/launch.json` "jsmanual", port 4321).
    Not published (GitHub Pages: later, with the CI).
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
- Temporal dead zone checks (engine change N, agreed by the user 2026-11-12, IMPLEMENTED the
  same day): `compiler/tdz.c`, a data-flow pass between resolve_variables and resolve_labels,
  turns `get_loc_check` / `put_loc_check` on surely initialized locals into `get_loc` /
  `put_loc` (same size; closures, derived `this`, eval functions untouched); BC_VERSION not
  bumped. Fix found with it: the `put_x get_x -> set_x` peephole no longer crosses a
  statement / return position (it merged the start and the return of `return x;`).
  `let` loop 363 -> 246 ms on the PC (= `var`); `tools/profile/workload.js` unchanged
  (noise +-2 %). Tests: `tests/tdz.test.js` (29 checks, in run-tests.sh).
- Property cache (experiment of 2026-11-12, REJECTED and removed, user): a fixed table of the
  last lookups of OP_get_field by (shape, name), own and prototype properties, validated at
  each use. Up to 82.6 % hits on the workload, no time gained on the PC (QuickJS's lookup is
  already one or two probes in the shape's hash table). Details: known-bugs.md "Speed".
- Interpreter (implemented, `quickjs-debug.h`, `core/debug.c`): debug dispatch table chosen per
  frame, JS_SetDebugHook, stepping flag, breakpoint/statement bitmaps, frame and slot access,
  exception reporting once per thrown value. caught/uncaught prediction: static, at the throw
  (see "Pause on exceptions"), done.
- Host tool `qjsx`: a single executable (host QuickJS + C modules: event loop, sockets, serial,
  SHA-256, compile API; commands written in JS, precompiled and embedded). Commands: build,
  deploy, debug (proxy, CDP server for VS Code), monitor, targets, device ls|reset|info, trace,
  run. Targets: `serial:COM3`, `serial:/dev/ttyUSB0@921600`, `tcp:esp.local:5555`.
  Project file `qjsx.json` at the project root (name, main module, source folders + globs,
  default target, output folder, number of packages kept, `env` of the program on a device). VS Code: attach configuration
  (js-debug target type to verify: node vs chrome), scripts announced with `file:///` URLs of
  local sources, `.d.ts` of native modules written by `qjsx build`; breakpoints kept by the
  proxy and re-applied on ScriptLoaded and after an ESP restart.
  Debugging startup code: `qjsx debug --restart` (see "Debugger step 4"), which replaced the
  persistent "wait for debugger at boot" flag (`--wait-debugger`, dropped for now).
- Runtime library (replaces quickjs-libc, written with the binding library, shared with the ESP
  where it makes sense): globals `console`, `setTimeout`/`setInterval`/`clearTimeout`,
  `TextEncoder`/`TextDecoder`; `Uint8Array` for bytes (no Node `Buffer`). Modules: `sys/fs` (sync),
  `stdlib/path` (JS, standard library), `sys/net` (TCP, async), `device/serial` (class
  `Serial`; PC ports today, UARTs on a board later), `sys/crypto` (`sha256`, `sha1`),
  `sys/compiler` (PC only); `process` is a global. Naming rule: when a function behaves exactly like Node's, use
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
- Names of the native modules (user, 2026-11-11, DONE the same day except `device/gpio`;
  replaces "native modules keep their short names"; `process` is a global, not a module:
  not renamed): two reserved prefixes, `sys/` and `device/`; no library may take
  these names (`qjsx mklib` and `qjsx build` refuse them); `qjsx build` lists every
  `sys/...` and `device/...` import in `natives` (checked by `qjsx deploy`; `qjsx run` on
  the PC refuses a module the host does not have); the firmware registers the full names.
  Renaming: `fs` -> `sys/fs`, `crypto` -> `sys/crypto`, `process` -> `sys/process`, `net` ->
  `sys/net`, `compiler` -> `sys/compiler` (PC only: the firmware does not have it),
  `serial` -> `device/serial`. `device/` = the hardware of the machine the program runs on:
  on the PC only `device/serial`; on a board `device/gpio`, then `device/i2c`, `spi`, `pwm`,
  `adc`, `rmt`, `ws2812`, `serial` (UART)... Never a chip name in a module name.
- `device/serial` (user, 2026-11-11, DONE on the PC the same day; UARTs of a board: not yet): one concept on the PC and on a board, a class
  `new Serial(port, { baudRate, ... })` with `onData`, `write`, `close`; the port is a system
  name on the PC ("COM3", "/dev/ttyUSB0"), a UART number plus pins on a board
  (`new Serial(1, { baudRate, tx, rx })`). Replaces the current function `open(path,
  { baudRate })` of the PC module (the tool's own `qjsx:device` and `targets` adapt).
  UARTs of a board (design agreed by the user 2026-11-12, IMPLEMENTED the same day for the
  ESP32 and the twin; `tests/serial.test.js`; not tried on a board: nothing wired, user):
  - options on both: `baudRate` 115200, `dataBits` 8 (5..8), `parity` "none" | "even" |
    "odd", `stopBits` 1 | 2; PC only `flowControl` "none" | "rts-cts"; board only the pins
    `tx`, `rx` (one at least), `rts` + `cts` together (= flow control); a pin not given is
    not wired; a pin on the PC or flowControl on a board is a TypeError;
  - `port()` REPLACES `path()` (unused): the name on the PC, the number on a board;
    `list()` on a board = the numbers of the free UARTs; the UART of the agent's link
    (UART0 on the ESP32) is refused with EBUSY, a UART open already EBUSY, no such UART
    ENOENT; a port is freed at the end of the program; `readLine` (promise) later, in JS
    in the standard library;
  - ONE module `common/device/serial_module.cpp` over a DRIVER (`common/hal/hal_serial.h`,
    a table of functions chosen at start by `setSerialDriver`, because the qjsx
    executable holds both the PC's ports and the twin's UARTs): PC
    `host/platform/serial_host.cpp`, ESP32 `ports/esp32/main/hal_serial_esp32.cpp`
    (event queue of the driver: a block on 10 characters of silence or 120 bytes; a
    closed UART gives its pins back with gpio_reset_pin), twin
    `host/twin/sim/twin_uart.cpp`;
  - twin: `"uarts"` and `"linkUart"` in the board JSON; each UART a TCP link AND a
    section in the page (log of the traffic "program wrote" / "received from page or
    TCP", text or hex, clear, input in text with a line ending or in hex); the page and
    TCP both reach the program; no timing; messages `twin.uart.data`, `twin.uart.send`,
    uarts in `twin.state`. Tried in the browser pane 2026-11-12.
  - PAGE OF THE TWIN (user, 2026-11-12): the board view is ONE FOLDING SECTION PER DEVICE
    (each part: BOOT, RGB...; USB; each UART but the agent's link), our own small
    component `host/twin/ui/src/controls/section.ts` (x4js has none): title, short
    summary kept visible when folded, open / folded kept by the browser. NO docking
    panel for the devices (only the board and the pins table are docking panels).
  - TCP links of the twin (user, 2026-11-12, IMPLEMENTED the same day,
    `host/twin/sim/twin_tcp.*`, `tests/twintcp.test.js`): the PAGE chooses the port of
    each UART and each USB serial function and turns its listening on or off; the twin
    keeps the choices in `<storage>/twin.json`; NOTHING listens by default; a port in use
    turns it off with the error; a USB function listens only while the device is started;
    `--uart-port` / `--usb-port` kept (tests, scripts): that group listens from the start
    on consecutive ports and nothing of it is saved that run. The rows of the USB
    functions say plainly whether the program uses them ("unused: the device is not
    started", "unused: the program did not declare it", "used, ..."), with an icon.
  - UARTs wired to real PC serial ports (user, 2026-11-13, IMPLEMENTED the same day,
    `host/twin/sim/twin_uart_serial.cpp`, `tests/twinwire.test.js`; not tried with a real
    port: none on the PC): a settings icon on the title line of each UART section
    opens a menu with two sub menus holding their fields, "TCP" and "PC port" (port,
    connect, real speed) (user: less room, edits in the menus; x4js pushes a sub menu
    that does not fit back over its menu, so the sub menus are narrow, on several
    lines); a refused choice (no port) is a Notification; the port is open while the program has the UART open, with its settings;
    real speed (default: the write waits the line time) or immediate (64 KB queue);
    TCP and PC port at the same time; `--uart-serial 1=COM5` and `"twin.uarts"` of
    qjsx.json wire from the start, not saved (command line wins); `twin.json` is now
    shared through `twin_choices.cpp` (one key per link: "uart1", "usb0", "serial1").
- `device/rmt` + `stdlib/rgbled` (user, 2026-11-11, DONE the same day and tried on the board:
  colors and a rainbow on GPIO48; `setHsv` / `fillHsv` added, user's idea), for the WS2812 LED of the
  YD-ESP32-S3 on GPIO48 (only connected when the "RGB" solder bridge of the board is closed):
  - `device/rmt` (native): `new Rmt(pin, { resolution })` (ticks per second), `send(pulses)`
    ([duration, level, ...] in ticks) and `sendBytes(bytes, { bit0: [high, low], bit1: [high,
    low], reset })`, both promises settled when the emission is over; `close()`. Same layers
    as gpio: `common/hal/hal_rmt.h`, ESP32 implementation with the ESP-IDF RMT driver (TX
    channel, bytes encoder, end-of-transmission callback posted to the loop), class in
    `common/device/`. RP2350 later through PIO. Reception later;
  - `stdlib/rgbled` (JS, standard library, on top of device/rmt): `new RgbLed(pin, { count,
    type })`, `set(index, r, g, b)`, `fill(r, g, b)`, `await show()`, `close()`; the LED type
    is an option (`type: "ws2812"` first: GRB order, 0.4 / 0.8 us timings): a new LED model is
    a table, no firmware change. The first JS layer of the standard library over a native
    module.
- `device/usb` + `stdlib/usb` (user, 2026-11-12, DONE the same day, tried on the YD-ESP32-S3
  native USB port; `doc/architecture.md` 5.4): the board as a COMPOSITE USB device for a host
  (first use: a client board whose tablet only speaks USB; that board's files are git-ignored):
  - `device/usb` (native): `new Usb({ manufacturer, product, serialNumber, vendorId,
    productId })` (printable ASCII, 31 chars max; IDs 0 = chip vendor's default), functions
    declared before `start()`: `usb.serial()` -> `UsbCdc` (`onData`, `onLineState({ dtr, rts
    })`, `write(string | bytes)`, `lineState()`), at most 2 (esp_tinyusb); one `Usb` at a
    time; `close()` stops the device. The device OUTLIVES the program: same descriptors and
    functions = taken over without re-enumeration, else restarted. A started device keeps the
    program alive until close();
  - `write()` (user, after the try on the board): DTR is NOT a condition (Windows opens with
    RTS only); what the host does not take in 50 ms is dropped without error;
  - `stdlib/usb` (JS): re-exports `Usb`, `UsbCdc`, adds `class UsbSerial extends Usb` (one
    serial function, started at once), so a program has one import;
  - ESP32: esp_tinyusb (component manager, `ports/esp32/main/idf_component.yml`, chips with
    USB OTG only), `hal_usb_esp32.cpp`; twin: each serial function is a TCP port
    (port chosen on the page, or `qjsx agent --usb-port <port>`), `tests/usb.test.js`. The PC cannot be a USB device: a USB
    relay board (an ESP32-S3 with a small C firmware) for a real tablet is in TODO.md.
- `device/adc` (agreed by the user 2026-11-12, IMPLEMENTED the same day; ESP32 firmware
  builds, NOT tried on the board): `new Adc(pin, { range = 3.3 })` (range: the highest
  voltage expected, in volts; the hal chooses the setting), `read()` raw (0..2^bits-1),
  `readVoltage()` volts (calibrated), `bits`, `close()`; synchronous; EINVAL for a pin
  without analog input or a range too high, EBUSY if open. `common/hal/hal_adc.h`,
  `common/device/adc_module.cpp`, `ports/esp32/main/hal_adc_esp32.cpp` (adc_oneshot, ADC1
  ONLY (user: ADC2 fails with WiFi), attenuation from the range, curve / line fitting
  calibration), twin `host/twin/sim/twin_adc.cpp` (saturates at the span, as the S3).
  RP2350 later. DAC: postponed (our boards have none; with the twin's oscilloscope).
- `device/pwm` (agreed by the user 2026-11-13, IMPLEMENTED the same day for the ESP32
  and the twin, `doc/architecture.md` 5.4, `tests/pwm.test.js`; ESP32 firmware built and
  flashed, VERIFIED on the YD board by the user with an oscilloscope, 2026-11-13): `new Pwm(pin, { frequency
  = 1000, duty = 0, invert = false })`, `setDuty(0..1)`, `setFrequency(hz)` (duty kept),
  `setPulseWidth(us)` (servos), fields `duty` / `frequency` / `bits` = what the hardware
  does (rounded); resolution chosen by the hal; a frequency no shared timer can take is
  EBUSY (never a change of the other outputs); `close()` AND the end of the program stop
  the output, pin driven at its idle level (unlike Gpio); fade and `stdlib/servo` later.
  `common/hal/hal_pwm.h`; `common/hal/pwm_timers.h` = the LEDC model (2^bits counter,
  divider 8.10 bits, timers shared by frequency) shared by `hal_pwm_esp32.cpp` (LEDC, APB
  80 MHz, low speed) and the twin's `twin_pwm.cpp` (board JSON `"pwm": { outputs, timers,
  clock, maxBits }`). Twin: same frequency = new duty from the next period (phase kept,
  as LEDC).
- Analyzer of the twin (user, 2026-11-13): PWM and RMT NOT recorded edge by edge: the twin
  records PWM changes (frequency, duty, invert, from a time) and RMT sends as sent
  (pulses, or bytes + bit timings), the page computes the edges for the window, the VCD
  (ns) for the file (cap 1,000,000 changes, Notification). Modes Roll / Normal / Single;
  trigger = an edge (rising, falling, both) of a shown pin, any edge known (levels, PWM,
  RMT), at 10 % of the view, dashed line, times counted from it, holdoff = one window;
  Auto, adjustable holdoff / position later. Tool bar on two lines (functions, then pins);
  Save / Clear in a context menu of the traces. Pins table columns resized by an x4js
  `Header`, the part column flex.
- `device/i2c` (API agreed by the user 2026-11-12, IMPLEMENTED the same day for the ESP32
  and the twin, `doc/architecture.md` 5.4, `tests/i2c.test.js`; not tried on a board: no I2C
  component; RP2350 later): `new I2c(port,
  { sda, scl, frequency = 100000, pullUp = false, timeout = 100 })`; `bus.device(address, {
  frequency, timeout })` -> `I2cDevice` (frequency PER DEVICE); transfers ONLY on devices:
  `write(bytes)`, `read(length)`, `writeRead(bytes, length)` (promises) AND `writeSync` /
  `readSync` / `writeReadSync`; bus: `probe` / `probeSync` (boolean), `reset()`, `close()`;
  bytes = Uint8Array or array of numbers; errors `ENXIO` / `ETIMEDOUT` / `EIO`; transfers in
  call order; atomic sequences (mux then read) left to the program. Inside: one "i2c" task
  shared by every bus runs the queued jobs (blocking hal), a Sync call waits for the jobs of
  its bus then runs on the JS thread; 16 promises pending per bus at most; a device finds
  its bus by number + generation. Twin: nothing answers. ENXIO added to jsbind's errno names.
- Firmware version (user, 2026-11-11, DONE): `QJSX_VERSION` is in `common/runtime/version.h`
  (moved from host/), the same for the tool and the firmwares; `agent::Identity.version`,
  sent in the HELLO answer (`VERSION`), shown by `qjsx device info` ("firmware: qjsx ...")
  and in "connected to ..."; `qjsx deploy` warns (does not refuse) when the tool and the
  firmware differ; ESP-IDF takes it as PROJECT_VER (ports/esp32/CMakeLists.txt parses the
  #define). Read from JS (user, 2026-11-12, DONE): `process.versions` = `{ qjsx, quickjs,
  build }` (a plain object, as Node's; `build` = the package's build id, a hash of its
  bytecode, set by startPackage for every package run; absent for `qjsx run script.js`).
- `device/gpio` (user, 2026-11-11, IMPLEMENTED the same day on the ESP32, see TODO): a class `new Gpio(pin, { mode, pull })`
  (`init(...)` of the binding library), mode "input" | "output" | "openDrain" (the
  JS_ENUM name: "open-drain" is refused, found 2026-11-13), pull "up" |
  "down" | "none" (JS_ENUM); `read()`, `write(level)`, `toggle()`, `close()`;
  `onChange(callback(level), { edge: "rising" | "falling" | "both", debounce })` (event);
  `waitFor(level, { timeout })` (promise). Pins by their GPIO number only (a table of names
  per board, "LED", "BOOT", maybe later). NO check of the pins (reserved, strapping): the
  program is responsible. Pins are NOT reset at the end of a program (user, 2026-11-11: an output
  keeps its state; only close() resets a pin); ESP-IDF then warns "gpio: conflict found for
  GPIO[n]" when the next program configures the pin again: harmless, kept. First test: toggle GPIO1 as fast as possible in a loop, the user
  measures the frequency with an oscilloscope (expect a ~1 ms gap every 100 ms: yieldIfDue).
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
  (recursive Mutex, Signal = mutex + binary semaphore, sleepMs). `gpio_watch.{h,cpp}`
  (2026-11-12): debounce + event task of hal::gpio::watch, shared by the ESP32 and RP2350
  hal_gpio (the port keeps registers + interrupt, calls gpiowatch::postFromInterrupt).
- RP2350 `device/gpio` (2026-11-12): `ports/rp2350/main/hal_gpio_rp2350.cpp` (SIO registers,
  native toggle, open drain made with the direction, bank 0 IRQ on the JS task's core).
  Build option `-DQJSX_JS_HEAP_IN_SRAM=ON` (measures: JS heap in SRAM, PSRAM unused), given
  to `tools/pico.ps1 configure`; stays in the CMake cache until set back OFF.
  Speed (2026-11-12, known-bugs.md "Speed"): `JS_CallInternal` (25 KB) is larger than the
  16 KB XIP cache; it and ~30 hot engine functions (`QJSX_SRAM_FUNCTIONS` in
  ports/rp2350/CMakeLists.txt) run from SRAM: objcopy renames their `.text.<name>` section
  of the engine library to `.time_critical.<name>` (no engine change). The engine library
  needs `-ffunction-sections -fdata-sections` itself (the pico-sdk only gives them to the
  firmware's own sources). Up to ×2.8 faster; the gpio15 toggle went 122 kHz -> 132 kHz
  (per clock now close to the ESP32-S3: ~570 clocks per toggle turn against ~490).
  interpreter.c is compiled with `-fira-region=one` (else GCC spills `pc` of
  JS_CallInternal to the frame, every opcode; same in upstream QuickJS): about -10 %.
  The same flag may help the ESP32 (Xtensa) build: not tried.
  Measures on the PC (user, 2026-11-12: "fais les tests en local pas sur les cartes"):
  `bash tools/profile/profile.sh [Profile]` runs `tools/profile/bench.js` with qjsx-tests
  (module `profile_test`, tests/native/profile_module.cpp: sampling thread with
  SuspendThread / GetThreadContext, Windows only; empty native functions to time calls),
  resolves addresses with addr2line. Findings in known-bugs.md "Speed" (`let` costs 30 %).
- `ports/rp2350/` — pico-sdk + FreeRTOS SMP project, one firmware per board (`boards/<board>/`:
  `board.cmake` = PICO_BOARD / PICO_PLATFORM, `board_config.h` = name, flash map, PSRAM).
  `main/`: `main.cpp` (PSRAM, USB, JS task on core 1, scheduler), `board_rp2350.cpp` (USB CDC
  link through stdio_usb, package slots in flash via flash_safe_execute, JS heap =
  RegionHeap in PSRAM), `psram_rp2350.cpp` (QMI CS1 set-up from RAM), `platform_rp2350.cpp`,
  `FreeRTOSConfig.h`. `compat/`: pthread / dirent / file calls newlib lacks. The engine is a
  separate static library (`qjsx-engine-target`): QuickJS's `list.h` vs FreeRTOS's.
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
  (`tools/embed-js.lua`), `twin/` = the digital twin (`sim/` C++, `js/` -> `qjsx:twin/<name>`,
  `ui/` x4js page embedded by `tools/embed-twin-ui.lua`).
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
  (or `config=debug`, `config=profile`). Configurations (user, 2026-11-12): Debug (-O0,
  debug info, asserts), Release (optimized, NO debug info, every executable stripped
  with -s: qjsx 10.9 MB -> 2.5 MB), Profile (the code of Release WITH its debug
  info: what tools/profile/profile.sh uses, addr2line needs it). Projects: `qjsx-engine` (engine/qjsx), `qjsx-engine-target`
  (the same without compiler/, QJSX_NO_COMPILER: no eval, like the targets), `qjsx` (the
  tool), `qjsx-tests`, `qjsx-target` (qjsx without its compiler module, linked with
  qjsx-engine-target: runs packages like a
  target; checked by run-tests.sh with tests/fixtures/nocompiler), `qjsx-runtime` (2026-11-12:
  the runner of `qjsx build --executable`, engine without compiler + runtime + PC native
  modules, its own main in host/runtime/, no tool JS, 1.25 MB in Release;
  the package is appended to a copy of it with a trailer "QJSXPKG1" + 64-bit LE size;
  argv = [exe, exe, args]). JS modules embedded as
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
- RP2350 (ports/rp2350): `tools/pico.ps1 [board=NAME] configure|build|flash|ninja ...|picotool
  ...` (toolchain in C:\tools\pico, CMake from the PATH; no special environment: runs from the
  PowerShell tool), builds in ports/rp2350/build-NAME, log C:\temp\pico-build.log (UTF-16),
  exit code C:\temp\pico-status.txt. `configure` again after adding / removing a source.
  `flash` = `picotool load -f -x` (reboots a board running qjsx into BOOTSEL by USB); the
  first flash needs the board in BOOTSEL by hand. Flashing acts on the board: ask first.
- Second target board: Olimex PICO2-XXL (RP2350B, 48 GPIO, 16 MB QSPI flash, 8 MB QSPI PSRAM on
  QMI chip select 1 = GPIO8, one USB-C, microSD, UEXT, BOOT + RESET buttons; schematic Rev. C in
  github.com/OLIMEX/RP2350-PICO2-XXL). Multi-platform plan AGREED 2026-11-11:
  ports/{cmake,freertos,esp32,rp2350}, a board.h interface (reset, identity, memory / PSRAM
  allocator, package storage, links), platform.h completed for an RTOS, one
  platform_freertos.cpp for both targets; order: engine without compiler validated on the PC,
  then ESP32-S3, then RP2350. The board.h / platform.h APIs are proposed to the user first.
  RP2350 port design AGREED by the user 2026-11-11; WRITTEN the same day (`ports/rp2350/`,
  architecture.md 5.5): builds (740 KB); FLASHED and TRIED 2026-11-12 (COM13 on the user's PC;
  the board had CircuitPython 9.2.2, overwritten with the user's agreement, no backup): deploy,
  run in place, 4 MB buffer in PSRAM, debugger 72/72, proxy 30/30, cdp 53/53. SLOW: 10^6 loop
  turns 4 s, 20,000 objects 3 s (known-bugs.md "RP2350 port"):
  - toolchain installed BY HAND in `C:\tools\pico` (user's choice; not the VS Code Pico
    extension): pico-sdk 2.x (+ TinyUSB), FreeRTOS-Kernel with the RP2350 Cortex-M33 port,
    Arm GNU Toolchain (arm-none-eabi-gcc), ninja, picotool; each download asked first;
    a `tools/pico.ps1` like `tools/idf.ps1`. CMake: the user's own, already in the PATH.
    INSTALLED 2026-11-11 in `C:\tools\pico`: `arm-gnu-toolchain-15.2.rel1/` (bin/arm-none-eabi-gcc
    15.2.1, zip SHA-256 checked), `pico-sdk/` (2.3.1, submodule lib/tinyusb only),
    `FreeRTOS-Kernel/` (V11.3.3 + submodule Community-Supported-Ports: port
    `GCC/RP2350_ARM_NTZ`; not the stale Raspberry Pi fork), `picotool/picotool/` (2.3.1, the
    layout find_package(picotool) expects), `ninja/` (1.13.2), `downloads/` (the zips). The
    pico-sdk knows the board: PICO_BOARD=olimex_rp2350_xxl (RP2350B, 16 MB flash, plain LED on
    GPIO25, no RGB LED, no PSRAM define: CS1 = GPIO8 goes in our board_config.h).
    ARM cores (not RISC-V);
  - `ports/rp2350/` on the model of `ports/esp32/` (`main/`, `boards/olimex-pico2-xxl/`,
    FreeRTOSConfig.h); FreeRTOS SMP, JS task (48 KB) pinned to core 1, I/O and USB on core 0;
    platform: time_us_64, watchdog_reboot, xTaskCreateAffinitySet + platform_freertos.cpp;
  - JS heap in the 8 MB PSRAM (QMI CS1 set up by our code, mapped at 0x11000000) through an
    allocator WRITTEN BY US (user: not TLSF). Allocator design AGREED 2026-11-11:
    `common/runtime/region_heap.{h,cpp}` (portable, also for the STM32H743 AXI SRAM later),
    two-level segregated free lists (TLSF idea, our own code): 8-byte block header, size
    copied at the end of free blocks (O(1) merge), 8-byte alignment, classes every 8 bytes
    up to 256 then powers of two split in four, bitmap search; realloc grows / shrinks in
    place; API init(start, size), allocate, reallocate, release, usableSize, stats(Stats
    *out) (used, peak, largest free block); out of memory returns nullptr (QuickJS throws),
    not fatal; JS thread only, no lock; unit tests in qjsx-tests, then an option to use it
    as the desktop JS heap for the whole Debug suite; PSRAM not answering at boot: message
    and JS heap in the normal malloc (SRAM). DONE 2026-11-11: `memory::RegionHeap` +
    `memory::jsAllocator(heap, locked)` (QuickJS's functions, counts and limit kept);
    `tests/region_heap.test.js` (37 checks, module `region_heap_test`); desktop option
    `QJSX_JS_HEAP_MB=<MB>` (board_host.cpp): the whole Debug suite passes on it. Deviation:
    when no class above serves a request, the class of the size itself is walked (else the
    largest free block could not be taken whole). SRAM for FreeRTOS,
    stacks, memory::allocate. Expect slower JS than the ESP32-S3 (QSPI PSRAM, XIP cache
    shared with flash): measure;
  - packages: two flash slots at fixed offsets read in place through XIP; flash writes stop
    the other core (flash_safe_execute), run from SRAM, data copied to SRAM first (PSRAM is
    unreachable while the flash is written), QMI CS1 setup restored after;
  - link: native USB CDC (TinyUSB) on the board's USB-C (user's choice, no UART first); the
    COM port vanishes at each chip reset: the host tool must reopen it;
  - order: minimal firmware (agent, deploy, run, debugger, same board tests as the ESP32),
    then PSRAM heap + microbench, then device/gpio (native SIO XOR toggle), then device/rmt
    on PIO.
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
- ESP-IDF minimal build (user, 2026-11-13): `ports/esp32/CMakeLists.txt` sets
  `MINIMAL_BUILD`, only `main` and its REQUIRES are built: an ESP-IDF component used by
  main (e.g. esp_wifi later) must be added to the REQUIRES of `ports/esp32/main/CMakeLists.txt`.
  `tools/idf.ps1` sets `PYTHONUTF8=1` (else the IDF profile warns about Unicode).
- Pitfalls met: `JS_NewTypedArray` reads offset/length even when argc is 1 (always pass 3
  arguments); `JS_ThrowPlainError` and `JS_NewModuleValue` do not exist in this QuickJS; on
  32-bit (ESP) JSValue is a plain uint64_t (never specialize templates on JSValue); members
  declared by JS_CLASS are private (go through ClassAccess); C++ names like `log`/`exit`
  clash with libc (use JS_FUNCTION_AS); in a void function write `return (void)js::fail(...)`.
- Engine tests and benchmark: `tools/quickjs-split/test/` (needs an extracted upstream release,
  https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz, for qjs.c, quickjs-libc.c, repl.js, tests/).

## Coding rules

- TYPINGS (user, 2026-11-13): every JS module has its hand-written `.d.ts` NEXT TO ITS
  SOURCE (`common/device/gpio_module.d.ts` next to `gpio_module.cpp`,
  `lib/stdlib/src/path.d.ts` next to `path.js`; the globals and `process` in
  `common/runtime/modules/globals.d.ts`). ANY change of what JS sees (a module, a class, a
  method, an option, a global, `process`) changes its `.d.ts` in the same work, types AND
  doc comments; a new module comes with its `.d.ts` (and its line in TYPING_FILES of
  `tools/embed-types.lua` if in a new folder). `tests/types.test.js` fails when names
  differ; types and comments are not checked: keep them right by hand. Where a module
  exists (PC, ESP32, RP2350, twin) is NOT written in its description (user, 2026-11-13)
  but in a `@compatibility` table, last before `@module` in the module comment (a block
  tag declared to TypeDoc: rendered at the bottom of the description), and in the table
  of `doc/jsmanual/api-readme.md`: update both when a port gains a module.

The full rules, with examples, are in `doc/coding-rules.md` (written to be reused in other
C / C++ projects). Summary of what was agreed:

- File header (user, 2026-11-11, APPLIED to 134 files the same day): every source file of
  the project starts with the block of `doc/coding-rules.md` 3 ("File header"): "qjsx" in
  ASCII art, "qjsx - JavaScript for devices", "Author: Etienne Cochard + Opus 5.5", "Copyright (c) 2026
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
