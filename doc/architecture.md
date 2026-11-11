# qjsx: design and reference

This document describes the architecture of qjsx: a JavaScript runtime based on QuickJS, for the
development machine (Windows, macOS, Linux) and for devices (see below), together with a
debugger usable from VS Code.

It gathers the decisions taken (recorded in [`CLAUDE.md`](../CLAUDE.md)), the description
of the split engine ([quickjs-map.md](quickjs-map.md)), the measurements
([quickjs-split-perf.md](quickjs-split-perf.md)), the generator
([tools/quickjs-split](../tools/quickjs-split/README.md)) and the task list
([`TODO.md`](../TODO.md)). If this document and `CLAUDE.md` disagree, `CLAUDE.md`
prevails.

**Device.** In this document, a *device* is a target board that runs the programs
(precompiled bytecode, debug and deployment agent, no compiler):

- the **ESP32** (ESP32-S3, YD board with a WROOM-1 module, PSRAM): ported, runs on the board;
- the **RP2350** (Olimex PICO2-XXL, RP2350B, 8 MB of PSRAM): planned, not written.

This list will grow: what is said of a device holds for any new target
(one more port in `ports/`, one implementation of `board.h`). A chip is named only
for what is specific to it (ESP-IDF, `esp_partition_mmap`, RP2350 boot ROM…).

## How to read this document

Each section carries an explicit status:

| Status | Meaning |
|---|---|
| **done** | the code exists in the repository and has been tested or measured |
| **in progress** | part of the code exists; the status block says what is done and what is not |
| **designed, not implemented** | the decision is taken, no code exists |
| **to verify** | a point that the sources explicitly mark as still to be confirmed |

The decisions marked as taken in `CLAUDE.md` are not reopened without the author of the
project.

## Contents

1. [Overview](#1-overview)
2. [The QuickJS engine](#2-the-quickjs-engine)
3. [The debugger](#3-the-debugger)
4. [Formats and protocol](#4-formats-and-protocol)
5. [The runtime](#5-the-runtime)
6. [The host tool `qjsx`](#6-the-host-tool-qjsx)
7. [Open points and points to verify](#7-open-points-and-points-to-verify)
8. [Appendices](#8-appendices)

---

## 1. Overview

> **Status: in progress** (2026-11-11). What runs today, on the PC and on an ESP32-S3 board:
> the engine (section 2), the packages and the libraries (4.1, 4.5), the device agent and its
> protocol (4.3), the runtime (5), the host tool (6) and the ESP32-S3 firmware (5.5). For the
> debugger, the part that lives on the device is done and tested (4.3); the host part (proxy,
> CDP server, VS Code) is not written (section 3). Not started: hardware modules (GPIO,
> I2C…), files on a device, native USB and WiFi links, the RP2350 port.

### 1.1 Goals

- Run modern JavaScript (promises, `async`/`await`, classes, modules) with QuickJS,
  on the development machine and on the devices.
- Debug from VS Code the code that runs on a device: breakpoints, stepping,
  variable inspection, expression evaluation, pause on exceptions.
- Deploy the compiled code to the device through the same channel as debugging.
- Write native modules (C++) visible from JavaScript easily.

"Development machine only" debugging is not a goal: the architecture aims at a remote
target.

### 1.2 Targets

| Target | Role |
|---|---|
| Development machine (Windows, macOS, Linux) | runs the host tool `qjsx` (compilation, package, deployment, debug proxy); can also run scripts |
| Device (ESP32, RP2350…) | runs the precompiled bytecode, the device agent (debug and deployment); no compiler, no `eval` |

### 1.3 Components

| Component | Where it runs | Role |
|---|---|---|
| QuickJS engine | host and device | interpreter; the compiler exists only on the host |
| Host tool `qjsx` | host | one executable: host QuickJS + C modules (event loop, sockets, serial port, SHA-256, compile API); its commands are written in JS, embedded as source and compiled when the tool starts. The standard library is delivered next to it (`stdlib.qjslib`) |
| Firmware + device agent | device | runs the scripts; the agent ("device agent") contains the debug stub and receives the deployments |
| Debug proxy (not written) | host, in `qjsx debug` | CDP adapter written in JS; translates CDP into agent messages; compiles the expressions to evaluate |
| VS Code + js-debug | host | debugger client, built into VS Code, speaks CDP |

### 1.4 The end-to-end flow

1. The developer writes JavaScript sources in a project described by `qjsx.json`.
2. `qjsx build` compiles the sources with the host compiler ("debug info" option), and
   produces a `.qjspkg` package: bytecode, debug sidecars, copy of the sources, manifest, with
   the library modules the program imports (4.5). Planned, not done: it also writes the
   `.d.ts` typings of the native modules for VS Code.
3. `qjsx deploy` sends the package to the device, through the agent, which writes it to
   flash. Today the whole package is sent, debug sidecars and sources included; a reduced
   package (same extension, the manifest says what it contains) is planned.
4. The device runs the bytecode, with no compilation on the target.
5. Planned, not written: `qjsx debug` starts the proxy. It opens a CDP server for VS Code
   (through js-debug) and talks to the device agent over a serial or TCP link. It finds the
   debug sidecars thanks to the build ids that the agent reports to it.

```mermaid
flowchart LR
    SRC[JS sources + qjsx.json] -->|qjsx build| PKG[Package .qjspkg]
    PKG -->|qjsx deploy| AG[Device agent]
    AG --> RUN[Bytecode execution]
    VSC[VS Code / js-debug] <-->|CDP| PROXY[qjsx debug: proxy]
    PROXY <-->|agent protocol: serial or TCP| AG
    PKG -.->|debug sidecars, sources| PROXY
```

### 1.5 Links between the host and the device

Three links are planned: serial (UART, COM/USB), WiFi and wired network (TCP). The debug
protocol is independent of the transport: the same framing applies everywhere (section 4.3).
Done today: the serial link of the ESP32-S3 (UART0), and TCP for the desktop agent that stands
in for a board.

---

## 2. The QuickJS engine

### 2.1 Version and choices

> **Status: done.**

- Engine: QuickJS by Fabrice Bellard, version **2026-06-04** (`engine/qjs-bellard/VERSION`). It is
  not quickjs-ng.
- mquickjs was evaluated and rejected: it is an ES5 subset (no `Promise`/`async`, no
  classes, no modules…).
- `engine/qjs-bellard/` contains the upstream engine, unmodified. The local patches of 2021 were
  dropped.

`quickjs.c` is 61,197 lines long: 1,562 functions, 142 types, 97 data tables. The map of the
modules of this file (line ranges, dependencies) is in [quickjs-map.md](quickjs-map.md),
and the list of every item in [quickjs-items.csv](quickjs-items.csv).

### 2.2 The split engine (`engine/qjsx/`)

> **Status: done.** Generated automatically, tested and measured (section 2.4), then modified
> by hand since 2026-11-10 (section 2.3 and [engine-changes.md](engine-changes.md)).

`engine/qjsx/` contains the same engine split into **55 source files** by the generator. The
hand changes added three files: `quickjs-debug.h`, `core/debug.c` and
`compiler/debug-info.c`. Each generated file
states at its top the lines of `quickjs.c` it comes from (for example `builtins/json.c`:
"Lines of quickjs.c: 23156-23360, 49092-50177").

#### Folders

| Folder | Role | Files |
|---|---|---|
| `core/` | always compiled | 18 files: atoms, bigint-arith, cfunc, closures, conversions, errors, gc, generators, interpreter, intrinsics, iteration, modules, objects, operators, print, properties, runtime, strings |
| `bytecode/` | loading and saving bytecode | opcodes, reader, writer |
| `builtins/` | standard objects, one file per object | 22 files: array, arraybuffer, atomics, base64, bigint, dataview, date, function, global, iterator, json, mapset, math, number, object, promise, proxy, regexp, string, symbol, typedarray, weakref |
| `compiler/` | the compiler, absent from a build without `eval` | 12 files: emitter, eval, finalize, funcdef, parser-class, parser-expr, parser-func, parser-module, parser-stmt, resolve-labels, resolve-vars, tokenizer |

The other files of the engine (`cutils`, `dtoa`, `libregexp`, `libunicode`, `list.h`,
`quickjs.h`, `quickjs-atom.h`, `quickjs-opcode.h`, `LICENSE`, `VERSION`) are copied as they are to
the root of `engine/qjsx/`.

The interpreter (`JS_CallInternal`, "computed goto" dispatch, `DIRECT_DISPATCH`) is in
`core/interpreter.c`. The frames (`JSStackFrame`) are chained from
`rt->current_stack_frame`.

#### Headers

- **`quickjs-internal.h`**, included by every file. Its parts, in order, are
  separated by marker comments:
  1. the prelude of `quickjs.c` (lines 1 to 1415: options, `JSRuntime`, `JSContext`, `JSObject`,
     `JSFunctionBytecode`, `JSStackFrame`…) without its prototypes;
  2. `/* ---- macros and types shared between files ---- */`;
  3. `/* ---- prototypes and shared data ---- */`: inclusion of `core/core.h`,
     `bytecode/bytecode.h`, `builtins/builtins.h`;
  4. `/* ---- inline functions shared between files ---- */`.
- **`core/core.h`, `bytecode/bytecode.h`, `builtins/builtins.h`**: prototypes of the functions and
  `extern` declarations of the tables shared between files.
- **`compiler/compiler.h`**: types (`JSFunctionDef`, `JSToken`, `JSParseState`, `BlockEnv`,
  `JSVarDef`…, `TOK_*` enumeration), macros and prototypes private to the compiler. Only the
  files of `compiler/` include it. Nothing outside `compiler/` calls the compiler
  directly, except `JS_NewContext` (in `core/runtime.c`) which calls `JS_AddIntrinsicEval`
  (in `compiler/eval.c`) through the public API.

#### Rules of the split

- Only the functions and tables used in several files lost `static`
  (688 functions, 48 tables); the others stay `static` in their file.
- The size of the shared tables is written in their `extern` declaration, so that `countof()`
  works elsewhere. If a table changes size without the header being updated, the compilation
  fails.
- A macro defined inside a function and used elsewhere (for example `JS_THROW_VAR_RO` in
  `JS_CallInternal`) is copied identically into a header.
- A few items are placed outside their original line range:

| Item | Destination | Reason |
|---|---|---|
| `free_function_bytecode`, `free_bytecode_atoms` | `core/gc.c` | free bytecode at run time, called by the GC |
| `JSOpCode`, `opcode_info` | `bytecode/opcodes.c` | used by the bytecode reader and writer (and later the debugger) |
| `JS_EvalFunction` and the eval dispatch | `core/interpreter.c` | run bytecode that is already compiled |
| `__JS_EvalInternal` | `compiler/eval.c` | single entry point to the compiler |
| `JS_ResolveModule` | `core/modules.c` | |
| `ident_realloc` | `core/strings.c` | |
| `get_line_col` | `core/errors.c` | |
| `js_get_prototype_from_ctor` | `core/objects.c` | |
| `add_export_entry` | `compiler/parser-module.c` | see change 2 below |

#### The generator

The split is produced by `tools/quickjs-split/split.js` (Node), driven by
`tools/quickjs-split/split-config.js`, and started by `tools/quickjs-split/regen.sh`.

Steps of `split.js`:

1. cut `quickjs.c` into top-level units (functions, data, types, macros,
   prototypes), each one remembering the `#if/#elif/#else` context it is in;
2. apply the agreed patches (JSON parser state, module exports);
3. assign each unit to an output file (line ranges + exceptions by name);
4. compute what is shared between files: the shared functions and data lose
   `static` and get a declaration in a header; the shared types, macros and inline functions
   go into a header;
5. write the files and print a report (number of files, shared functions and data,
   compiler functions used outside `compiler/`, unassigned units).

`split-config.js` contains:

- `version` (`2026-06-04`) and `preludeEnd` (1416: the lines before it become
  `quickjs-internal.h`);
- `fileRanges`: a list of `[first line, file]`, each range running up to the next
  entry;
- `overrides`: the items placed outside their range (table above);
- `patches`: the two patches described in 2.3.

`regen.sh` empties the content of `engine/qjsx/{core,bytecode,builtins,compiler}`, runs
`split.js` on `engine/qjs-bellard/quickjs.c`, then copies the other files of the engine.

**Rules of use:**

- `engine/qjsx/` was generated, and now carries changes made by hand: a regeneration would
  erase them, so `regen.sh` refuses to overwrite the folder without `--force`. To move to a
  new version of QuickJS, regenerate in another folder, then reapply
  [engine-changes.patch](engine-changes.patch). The author of the project must be asked
  before the engine is regenerated, or changed outside the work already agreed.
- The line ranges of `split-config.js` refer to the `quickjs.c` of version
  2026-06-04. For another version, they must be checked again.

### 2.3 Changes from the original

> **Status: done.** These are the only two changes made by the generator. The
> changes made by hand for the debugger (since 2026-11-10) are described in
> [engine-changes.md](engine-changes.md); their exact diff is in
> [engine-changes.patch](engine-changes.patch).

1. **JSON.parse has its own state.** In the original, `JS_ParseJSON3` and the JSON tokenizer
   reuse the state of the JavaScript parser (`JSParseState`, `js_parse_init`, `free_token`,
   `js_parse_error`, `js_parse_error_pos`). The patch adds to `builtins/json.c` a state
   of its own (`JSONParseState`, `JSONToken`, `JSON_TOK_*`, `json_parse_init`, `json_free_token`,
   `json_parse_error`, `json_parse_error_pos`) and renames the uses in the JSON ranges. JSON
   therefore no longer depends on the compiler.
2. **Module exports are separated from the parser.**
   - `add_export_entry2` (`core/modules.c`) no longer receives a `JSParseState` and throws an
     ordinary `SyntaxError` ("duplicate exported name");
   - `add_export_entry` (`compiler/parser-module.c`) reports the duplicate with its position in the
     source, then calls `add_export_entry2`;
   - the call in `JS_AddModuleExport` is adapted to the new signature.

The error messages and positions of `JSON.parse` and the duplicate export error are
identical in both versions (section 2.4).

### 2.4 Validation and performance of the split

> **Status: done** (measurements of 2026-11-10, details in
> [quickjs-split-perf.md](quickjs-split-perf.md)).

Conditions: Windows, gcc 13.1.0 mingw64, `-O2 -fwrapv`, no LTO, 20 threads; both
`qjs.exe` are built with the same `qjs.c`, `quickjs-libc.c` and `repl.c` by Bellard.

- **Tests**: Bellard's tests pass with both versions (`test_closure`,
  `test_language`, `test_builtin` with `--std`, `test_loop`, `test_bigint`,
  `test_cyclic_import`, `test_worker`).
- **Compilation**: 47.4 s for the original whatever the number of threads; 7.4 s for the
  split version on 20 threads. A change to `json.c` recompiles in 1.6 s (against
  42.7 s), a change to `quickjs-internal.h` in 6.7 s.
- **Size**: machine code 1,000 KB against 1,112 KB; `qjs.exe` without symbols 1,143 KB against
  1,256 KB.
- **Speed** (`tests/microbench.js`): −3.8% on the total, with per-test differences from −48.4%
  (`array_read`) to +27.5% (`prop_clone`). The explanation put forward (gcc no longer inlines
  across files the `static` functions called once) is an **unverified hypothesis**.

The tests need an extracted release of QuickJS 2026-06-04
(https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz) for `qjs.c`, `qjsc.c`,
`quickjs-libc.c`, `repl.js` and `tests/`. The procedure is in
[tools/quickjs-split/README.md](../tools/quickjs-split/README.md).

### 2.5 Removing the compiler on the device

> **Status: engine without compiler done and validated on the PC on 2026-11-11** (change H of
> [engine-changes.md](engine-changes.md): `qjsx-engine-target`, `qjsx-target`, fixture
> `nocompiler`), then built for the ESP32-S3. The firmware no longer embeds any JS: the
> library modules written in JS (`stdlib/path`) travel as bytecode in the program's
> package (`.qjslib` libraries, section 4.5).

Decision: **no `eval` on the device, debug builds included.** The `compiler/` folder is not
compiled for the device. The JS code arrives on the target already compiled by the development
machine. Everything the debugger evaluates (`Runtime.evaluate`, `evaluateOnCallFrame`,
`callFunctionOn`) is compiled on the host and sent as bytecode.

What the split already allows:

- the compiler (about 13,300 lines, 22% of `quickjs.c`) is isolated in `compiler/` with
  its private header;
- it is already behind an indirection: `ctx->eval_internal`, filled only by
  `JS_AddIntrinsicEval` with `__JS_EvalInternal`. If the pointer is NULL, `JS_EvalInternal` throws
  a `TypeError` "eval is not supported"; this is the path of `JS_Eval`, `JS_EvalThis`,
  `JS_EvalObject`, of the global eval, of the `Function` constructor and of the direct eval;
- `JS_NewContext` called `JS_AddIntrinsicEval`, the only dependency of the rest of the engine
  on the compiler: with `QJSX_NO_COMPILER` defined it no longer does (change H);
- the couplings identified in [quickjs-map.md](quickjs-map.md) (JSON, `free_function_bytecode`,
  `opcode_info`, `js_parse_error` in the exports, mixed roles of the `eval-api`, compiler
  types) are handled by the split (sections 2.2 and 2.3).

The ESP32-S3 firmware is such a build (section 5.5); on the PC, the executable `qjsx-target`
is the tool linked with the engine without compiler, used by the tests to run packages like
a device does.

### 2.6 Compiler changes

> **Status: done on 2026-11-10, with three deviations.** Up-to-date description, files touched and
> tests in [engine-changes.md](engine-changes.md). The text below is the original
> design. Deviations:
> - the positions go through the operand of `OP_line_num` (two bits for the kind), not through a
>   new opcode;
> - the scopes are **source** ranges, not pc ranges;
> - no build id in the bytecode and no per-script table in the reader: a script is
>   identified by the build id of the package and the module name, which is now always present.

- **Statement start positions.** Today, a `pc2line` entry is recorded only
  when `resolve_labels` emits instructions (`add_pc2line_info`). A temporary opcode
  `OP_stmt_pos <offset>` will be emitted:
  - at the start of each statement;
  - at each `catch` clause;
  - at the entry of the function body;
  - at each `return` and at the implicit return;
  - at each loop condition.

  `resolve_labels` attaches it unconditionally to the next emitted instruction. Each
  position carries a kind: statement, return or expression.
- **Variable scopes.** Scope entry and exit markers (without code) are
  passed down to `resolve_labels`, to get the final pcs and therefore the visibility range of
  each variable. Internal variables are flagged; slots in the temporal dead zone
  (TDZ) are reported as such.
- **Function number.** Each `JSFunctionBytecode` gets a 16-bit number (creation
  order), serialized: the version number of the bytecode format changes.
- **Build id.** The top-level function carries a build id, computed by the
  packer on the serialized bytecode with this field set to zero.
- **Function table at read time.** The bytecode reader builds, per script, a
  record (build id + number → function table, entry cleared when the function is
  freed) used by the stub.
- **`debugger;`** compiles to a new opcode `OP_debugger` (no effect without a debugger
  attached). Today, the keyword is ignored by the parser ("currently no debugger").
- **Tail calls always disabled**, in debug as in production: same bytecode in
  both cases, complete call stacks.
- **Collection of the debug sidecar** in `js_create_function`, before the information is
  stripped; the host compiler returns `{ bytecode, debug }` to the host JS, which writes the JSON,
  the manifest and the zip (CRC32 computed in JS).

### 2.7 Interpreter changes

> **Status: done on 2026-11-10, except `sf->call_sp`** (prediction of uncaught
> exceptions, not done). Public API in `engine/qjsx/quickjs-debug.h`, details in
> [engine-changes.md](engine-changes.md), section F. The debugger stub of the device agent
> uses it (section 4.3).

- **Two dispatch tables** (option C): a normal table and a debug table. The debug
  table checks a per-function bitmap of the stop positions. When "computed goto" is
  not available, a per-opcode test is the fallback. Details in 3.2.
- **`sf->call_sp`**: a new field of `JSStackFrame`, saved before each call, for
  the prediction of uncaught exceptions (3.4). The existing field `cur_sp` does not fit:
  it already has a meaning for generators.
- **`OP_debugger`** to execute (see 2.6).

Existing entry points useful to the debugger (lines of the original `quickjs.c`, see
[quickjs-map.md](quickjs-map.md)): `JS_CallInternal` (hooks), `JSStackFrame`, `find_line_num`
(`pc2line` decoding), `build_backtrace`, `compute_pc2line_info`, `js_poll_interrupts` and
`JS_SetInterruptHandler`.

### 2.8 Build

> **Status: done.**

- Toolchain: Windows, gcc 13.1 mingw64, premake5 5.0.0. The makefiles are
  generated with `premake5 gmake --cc=gcc --shell=posix`, then `make -C build config=release`
  (or `config=debug`).
- `premake5.lua` builds the engine as two static libraries, `qjsx-engine` and
  `qjsx-engine-target` (the same without `compiler/`, with `QJSX_NO_COMPILER`), and the tool
  (section 6). `CONFIG_VERSION` comes from `engine/qjsx/VERSION`.
- The devices build the same engine sources through `ports/cmake/qjsx.cmake` (section 5.5).
- The test build that compares the original engine and the split one is apart, in
  `tools/quickjs-split/test/` (it needs node for its benchmark table).

---

## 3. The debugger

> **Status of the whole section: in progress.** This section is the design. What exists
> today (2026-11-11):
> - **done, on the device**: the debugger stub of the agent (`common/agent/debugger.*`):
>   breakpoints and statement stops, pause of a running program, stepping, pause on every
>   exception, stack, slots and properties, changing a variable, loading and calling
>   bytecode compiled on the host. Messages in 4.3; tested through the desktop agent and on the
>   ESP32-S3 (`tests/debugger.test.js`, 52 checks on the board; 72 since the stepping by
>   activation number of 3.3, checked on the desktop agent only);
> - **done, on the host**: the core of the proxy, without CDP (2026-11-11):
>   `host/js/debuginfo.js` (packages indexed by build id, sidecars, byte offsets ↔ lines and
>   UTF-16 columns, where a breakpoint goes, visible variables by scope) and
>   `host/js/debugsession.js` (a session with a device: breakpoints by line kept and set again
>   at each `SCRIPT_LOADED` / `STARTED`, stack with names and lines, variables by name,
>   evaluation as in 3.5, steps). Lines and columns from 0, as in CDP. Tested by
>   `tests/debuginfo.test.js` and `tests/proxy.test.js` (desktop agent);
> - **done, on the host**: the CDP server (2026-11-11): `qjsx debug`, `host/js/websocket.js`
>   (HTTP + WebSocket) and `host/js/cdp.js` (3.9). Tested by `tests/websocket.test.js` and
>   `tests/cdp.test.js` (a client that plays js-debug's part; desktop agent and ESP32-S3).
>   Tried in VS Code itself on 2026-11-11 (desktop agent);
> - **not done**: the "uncaught" prediction (3.4), conditional breakpoints and logpoints
>   (3.6: refused by the CDP server until then), waiting for the debugger at startup (3.8).

### 3.1 Architecture

- **Client**: VS Code, through its built-in debugger js-debug. The protocol on the VS Code
  side is therefore **CDP** (Chrome DevTools Protocol), not DAP.
- **C stub on the target**: minimal, it exposes primitives (frames, variable
  slots, properties, loading and calling bytecode). It is part of the
  device agent, which also handles deployment.
- **Proxy on the host**: the CDP adapter is written in JavaScript and runs in the host
  QuickJS (the `qjsx` executable, not Node). This host QuickJS embeds the compiler: it compiles the
  evaluated expressions and produces the bytecode and the debug sidecars of the scripts.
- **C modules the host needs**: `quickjs-libc` has no sockets and its Windows event
  loop only waits on stdin, timers and workers. The host therefore needs
  C modules for an event loop, TCP sockets and the serial port (section 5).
- **Split debug info**: since the code arrives precompiled,
  the debug info lives on the host, next to the proxy, in a sidecar, and not in the target's
  bytecode. A script is identified by the build id of its package and its module name (3.7).

Division of roles:

| Role | Stub (device) | Proxy (host) |
|---|---|---|
| Stopping on a breakpoint, stepping | detects the stops (dispatch tables, bitmap) | translates lines/columns into positions, computes the bitmaps |
| Variable names, scopes | does not know them | reads them in the debug sidecar |
| Expression evaluation | runs the bytecode it receives | compiles the wrapped expression |
| Conditions, logpoints, counters | evaluates on the spot | compiles |
| Persistent breakpoints | | keeps them and re-applies them |

### 3.2 Breakpoints

**Decision: two dispatch tables (option C).**

- The interpreter has a normal dispatch table and a debug table. The debug
  table checks, for the current function, a **bitmap of the stop positions**.
- Without "computed goto", a per-opcode test is the fallback.
- Opcode patching (replacing an instruction with a stop instruction) was **rejected**:
  the bytecode may be in ROM, and patching complicates stepping.
- The possible stop positions are the statement start positions produced by the
  compiler (2.6).
- The proxy sends the stop positions with the `SET_STOPS` message. It keeps the list of
  breakpoints and re-applies them at each `SCRIPT_LOADED` and after a device restart.

### 3.3 Stepping

All the rules are expressed as "next statement start":

| Command | Stop at the next statement start… |
|---|---|
| over (main step) | in the same frame or in a calling frame |
| into (enter) | anywhere; native functions are stepped over, JS callbacks called from C are entered |
| out (leave) | in a calling frame |

- **Frame comparison** (decided 2026-11-11, replaces the address in the C stack and the stack
  depth): by the **activation number** the engine gives each frame
  ([engine-changes.md](engine-changes.md), L). It is unique and never reused: a frame created
  later at the same place of the C stack never passes for an earlier one. The frame of a
  generator or an `async` function keeps its number across `yield` / `await`.
- **What the stub records when a step starts** (`startStep`, `common/agent/debugger.cpp`):
  - over: the start frame, unless the pause is at its return position (it is ending);
  - over from an `async` function: only that frame. An `await` suspends it and a promise job
    resumes it later, with other callers: the step waits for it, so a step "over" an `await`
    resumes in the same `async` function. A generator suspended by `yield` goes back to its
    caller, which is recorded;
  - otherwise, over and out: every calling frame that runs bytecode.
- **Where the step stops** (`shouldStopStepping`): at a statement start of a recorded frame.
  While a recorded frame is still on the stack below, the code it called runs without
  stopping. When none is left (end of a callback of the event loop, out of an `async`
  function resumed by a promise job, uncaught exception), the step goes on as an "into", as
  V8 does: it stops at the first statement run outside the start frame.
- **Known limit**: an `async` function that ends with an exception after a step "over" from
  inside it is never resumed; stepping stays on (slower program) until the next pause.
- **Additional stops**: at the start of a `catch` clause, and at function end (at each
  `return` and at the implicit return) to show the return value.

### 3.4 Pause on exceptions

Two modes, set by `SET_EXCEPTIONS`. Today the stub pauses in the "all" mode only; the
"uncaught" mode never pauses, because its prediction is not done:

- **"all"**: pause at the `throw`, before the stack is unwound.
- **"uncaught"**: the prediction is made at the `throw`, by scanning the frames
  for `catch` offsets. The following do not count as handlers:
  - offset 0 (iterator close);
  - a handler that starts with `gosub; throw` (rethrow of a `finally`).

  This prediction needs the new `sf->call_sp` (2.7).
- **Async boundaries**: an exception that crosses the boundary of an `async` function
  counts as caught. Unhandled promise rejections are reported (console, stack captured
  at the `throw`) through the "promise rejection tracker".
- An uncaught exception of the program is printed on its error output (a `LOG` message
  during a session), and the program goes on. A dedicated event is not done.

### 3.5 Expression evaluation

- The proxy wraps the expression in a function whose parameters are the variables
  visible at the stop position (known from the debug sidecar), compiles it on the host, then the
  stub runs it (`LOAD_FUNCTION` then `CALL`).
- This covers `Runtime.evaluate`, `Debugger.evaluateOnCallFrame` and `Runtime.callFunctionOn`.
- The js-debug helper functions are loaded only once (`LOAD_FUNCTION`).
- Accessors (getters) are never run implicitly during a `GET_PROPERTIES`.
- Changing a variable goes through `SET_SLOT`.

### 3.6 Conditional breakpoints, logpoints, hit counts

Conditions, logpoints and hit counts are compiled by the proxy and
**evaluated on the device itself**, with no round trip to the host at each hit.

### 3.7 Source / bytecode / sidecar consistency

- Each script has a build id: the first 32 hexadecimal digits of the SHA-256 of its
  bytecode. It is recorded in the manifest of the package and in the sidecar, not in the
  bytecode itself.
- The device reports the build id of the package it runs (`STARTED`, `INFO`) and the name of
  each module it loads (`SCRIPT_LOADED`, `LIST_SCRIPTS`): a script is identified by both.
- The proxy indexes the `.qjspkg` packages it finds to find the package, then the sidecar of
  the module.
- The proxy checks the consistency between the source, the bytecode and the sidecar (the manifest
  contains the hash of the source).
- The scripts are announced to VS Code with `file:///` URLs pointing to the local sources.

### 3.8 Waiting for the debugger at startup (`--wait-debugger`)

- The agent holds `main` until the proxy gives the go-ahead, so that the startup code
  can be debugged (opening the serial port often resets ESP32 boards).
- The setting is remembered by the agent for the next startup.
- It has a timeout.
- It is never used on production devices.

### 3.9 The CDP server (`qjsx debug`)

> **Status: done on 2026-11-11** (`host/js/websocket.js`, `host/js/cdp.js`, command in
> `host/js/cli.js`), tested by a client that plays js-debug's part; not tried in VS Code
> itself yet.

`qjsx debug [--target t] [--port p] [--host a]`, in a project folder, opens the device and
listens (default 127.0.0.1:9339, `debugPort` of `qjsx.json`; not Node's 9229, which a Node
debugging session may use at the same time). VS Code attaches to it as to a Node.js
inspector (configuration in 6.5).

What js-debug does, read in its sources (main branch, 2026-11-11):

- it asks `GET /json/version` and `GET /json/list` and takes `webSocketDebuggerUrl` (or
  `websocketAddress` from the configuration), then opens the WebSocket;
- an error answer to a command is almost never fatal: the commands the server does not
  serve get "not found" (`-32601`);
- scripts are expected as `file:///` URLs; with a `hash`, js-debug would compare the local
  file and show a read-only copy if they differ: no hash is sent;
- breakpoints come as `Debugger.setBreakpointByUrl` with a JS `urlRegex`, matched against the
  URL of every module; the answer gives `breakpointId` and `locations`, and
  `Debugger.breakpointResolved` follows for the modules loaded later;
- destroying the default execution context closes its session.

What the server does:

- **One client at a time**; a new one replaces the previous one. The device is attached
  (`DEBUG_ATTACH`) when the client first needs it, and detached when it leaves (stops
  removed, a paused program resumes).
- **The session survives the end of the program**: the default context is never destroyed.
  A new run (redeploy, device restart) clears and recreates the context, announces its
  scripts again and binds the breakpoints in the new package.
- **Scripts**: a program module is announced with the URL of its file in the project; a
  library module with a copy of its source written in `<output>/debug-src/` (a URL without
  scheme would be taken by js-debug for a Node internal, and skipped).
- **Identifiers**: `scriptId` per module and per run; `callFrameId` and scope `objectId`
  carry the number of the pause, so that those of a pause that is over are refused;
  `objectId` "h:<handle>" for an object of the device.
- **Commands**: `Runtime.enable` (context 1, `isDefault`), `Debugger.enable`,
  `setBreakpointByUrl` / `setBreakpoint` / `removeBreakpoint` / `getPossibleBreakpoints` /
  `getScriptSource`, `pause` / `resume` / `stepInto` / `stepOver` / `stepOut`,
  `setPauseOnExceptions` ("caught" counts as "all"), `evaluateOnCallFrame`,
  `Runtime.evaluate`, `Runtime.callFunctionOn` (compiled on the host, loaded once per source
  text in the persistent group: js-debug's helpers), `Runtime.getProperties` (scopes and
  objects; getters never called), `Runtime.releaseObject`, `Debugger.setVariableValue`.
  `returnByValue` goes through `JSON.stringify` on the device. Accepted and ignored:
  `Profiler.enable`, `NodeWorker.enable`, `setAsyncCallStackDepth`, `setBlackboxPatterns`,
  `setBreakpointsActive`, `runIfWaitingForDebugger`, the `disable` commands.
- **A breakpoint with a condition, and a logpoint, are refused** until 3.6 is done (VS Code
  shows them unbound).
- **Program output** goes to the debug console as `Runtime.consoleAPICalled`, one message
  per line; the end of the program as an "info" message.
- **Not done**: previews of objects, `this` of the frames (always `undefined`), async stack
  traces, the "global" scope.

---

## 4. Formats and protocol

### 4.1 The `.qjspkg` package

> **Status: done** (`qjsx build`, `host/js/build.js` and `zip.js`; reading
> `common/runtime/package.*`). Current content:
> - `manifest.json`: `format` (1), `name`, `buildId`, `created`, `compiler`
>   (`"qjsx 0.1.0 / QuickJS 2026-06-04"`), `main`, `scripts[]` with, for each one, `path`,
>   `module`, `buildId`, `sourceHash`, and the names of its `bytecode`, `debug`, `source` entries;
>   a module that comes from a library also carries `library` (section 4.5);
> - `libraries[]` (name and version of the libraries used) and `natives[]` (native modules
>   imported, which the firmware must provide);
> - `bytecode`: the bytecode version number of the package (see "Package compiled for
>   another engine" below);
> - `bytecode/<path>.qbc`, `debug/<path>.json`, `src/<path>`;
> - ids: build id of a script = first 32 hexadecimal digits of the SHA-256 of its
>   bytecode; `sourceHash` = SHA-256 of the source; build id of the package = SHA-256 of the
>   ids of the scripts and of the main module; file `<name>-<12 digits>.qjspkg`;
> - zip with stored entries, fixed dates (1980-01-01): same content, same archive.
>
> The bytecode is compiled with every debug info stripped (`strip: "all"`, `JS_STRIP_DEBUG`):
> no source text, positions nor variable names. The engine keeps the module name outside the
> debug info (change A of [engine-changes.md](engine-changes.md)), so `import.meta` still
> works. There is no build id inside the bytecode: a script is identified by the build id of
> its package and its module name.

- **One zip per build**, extension `.qjspkg`. Today there is one kind of package, the full
  one, and it is what `qjsx deploy` sends. A reduced package for the device (same extension,
  the manifest saying what it contains) is planned, not done.
- **Entries stored without compression**: no dependency on a compression library, and
  the device reads the package in place. On the ESP32 the slot of the package is mapped with
  `esp_partition_mmap` and the modules are read from there, without copying the package into
  RAM. The engine still copies the bytecode it loads: `JS_READ_OBJ_ROM_DATA`, which would let
  it run the bytecode straight from flash, is not used. **To try.**
- The zip (with its CRC32) is written by the host JS.

Content:

| Entry | Content |
|---|---|
| `manifest.json` | format version, build id, compiler id, bytecode version, main module, `env`, libraries, native modules, list of the scripts (path, script build id, source hash) |
| `bytecode/` | the bytecode, that is, what goes to the device |
| `debug/` | one JSON debug sidecar per script |
| `src/` | exact copy of the compiled sources |

The exact names of the manifest fields are in the status block above; every hash is a
SHA-256.

**Package compiled for another engine** (done on 2026-11-11). An engine only reads the
bytecode of its own version (QuickJS's `BC_VERSION`, `0x45` today, given by
`JS_GetBytecodeVersion()`). `qjsx build` writes this number in the manifest (`bytecode`), and
it is checked in two places:

1. **When sending.** `qjsx deploy` puts it in `UPLOAD_BEGIN`. The agent compares it to its own and
   refuses at once if it differs: nothing is written, the installed package stays in place, and
   the tool prints the device's message ("the package holds bytecode version 70, this device
   reads version 69: rebuild the package, or flash a firmware that matches qjsx").
2. **When starting.** The loader (`startPackage`, `common/runtime/program.cpp`) reads the
   number again in the manifest before loading any module. This check does not depend on
   what the host says, and covers `qjsx run package.qjspkg` on the PC.

A package without a `bytecode` field (built by an older `qjsx`) is refused in the same
way. The number is only worth something if `BC_VERSION` increases at each engine change that
touches the bytecode: this is a rule of `engine-changes.md` (section K).

**`eval` and `Function` warnings** (done on 2026-11-11). On a device, `eval(...)`
and the `Function` constructor throw "eval is not supported"; on the PC they work.
`qjsx build` and `qjsx mklib` therefore warn, without failing:

```
src/main.js:46:29: warning: eval() needs the compiler, which a device does not have: it throws there
```

The detection is done by the compiler (change K) and it is syntactic: the direct
call `eval(...)`, the call `Function(...)` and `new Function`. An indirect use
(`(0, eval)(x)`, `globalThis.eval`, an alias) is not seen; the run-time error remains the
safety net. A module taken as it is from a library is not recompiled: its warnings
were given by `qjsx mklib`.

### 4.2 The debug sidecar

> **Status: done** (format 2, 2026-11-10). Details in
> [engine-changes.md](engine-changes.md), section E.

One JSON sidecar per script, in `debug/`, collected by the compiler before the information is
stripped: `format`, `path`, `buildId`, `sourceHash`, and `functions`. The index of a
function in `functions` is its function number. Each function carries:

- its name, its source range and the size of its bytecode;
- `positions`: triplets `pc, source offset, kind`, sorted by pc. The kind is 0 for
  an expression (the positions of the original pc2line), 1 for a statement start and 2 for
  a return;
- `arguments`, `locals` (with their scope), `closures`: names and flags (lexical, const,
  captured, global). The index of a variable is its slot in the frame;
- `scopes`: parent and **source range** of each scope. Deviation from the original
  design, which planned pc ranges: the proxy goes from the pc of the stop to a source
  position, then to the scopes that contain it. Internal variables are recognized by their
  name (`<ret>`, `this`, …). A variable in the TDZ reads with the tag `JS_TAG_UNINITIALIZED`.

The positions are **byte offsets in the source**. The proxy derives from them the lines and
the UTF-16 columns that CDP expects.

The link with the bytecode goes through the build id of the script (4.1, 3.7). Variable names
never travel to or from the device: they are read in the sidecar.

### 4.3 The device agent protocol

> **Status: done**, for deployment and for the device side of the debugger. Two
> implementations that must stay equivalent: JS on the host side (`host/js/cbor.js`,
> `frames.js`, `protocol.js`, `device.js`, `deploy.js`) and C++ on the device side (`common/agent/`:
> `cbor`, `frames`, `protocol.h`, `agent` — portable code, for the device and for the desktop agent
> `qjsx agent`). Tested end to end between the two (`tests/run-tests.sh`).
>
> Points fixed at implementation time:
> - **frame**: `0x00 | COBS(0xE5 | version 1 | CRC-16/CCITT-FALSE of the message, 2 bytes
>   big-endian | message) | 0x00`; on a serial link, a segment that does not decode as a
>   valid frame is console text;
> - **message**: CBOR array `[type, id, content]`; `id` > 0 for a request and its response,
>   0 for an event; content = integer-keyed map (or `null`);
> - **types**: `RESPONSE` 1, `ERROR` 2 (`{MESSAGE}`), `HELLO` 10 (`{PROTOCOL, TOKEN?}` →
>   `{PROTOCOL, AGENT, ENGINE, MAX_MESSAGE}`), `INFO` 11 (→ `{RUNNING, BUILD_ID, PACKAGE_SIZE}`),
>   `UPLOAD_BEGIN` 20 (`{SIZE, SHA256, BUILD_ID, BYTECODE}`, refused if the bytecode number
>   is not that of the device's engine), `UPLOAD_CHUNK` 21 (`{OFFSET, DATA}`, 2,048
>   bytes, in order), `UPLOAD_END` 22 (checks size and SHA-256, installs), `RUN` 23,
>   `STOP` 24, `RESET` 25; events `LOG` 30 (`{STREAM, TEXT}`), `EXITED` 31 (`{CODE}`),
>   `STARTED` 32 (`{BUILD_ID}`). Key values: `common/agent/protocol.h`;
> - **session**: any request other than `HELLO` is refused before `HELLO`; during the
>   session, the program's console (`console.*`, uncaught exceptions) goes out as `LOG`;
> - **retransmission**: the host resends a request without a response after 3 s (3 tries) with the same
>   id; the agent then resends the remembered response without executing again;
> - **token**: if the agent is started with a token, `HELLO` must provide it.
>
> **Debug part: done on the device side on 2026-11-10** (stub, `common/agent/debugger.*`). The
> messages and the encoding of values are described at the top of `host/js/protocol.js`.
> - **requests**: `DEBUG_ATTACH` 40 to `RELEASE` 54. They include pause, resume, the steps,
>   the stops per function, the exceptions mode, the list of modules, the stack, reading and
>   writing the slots, the properties and internal slots, loading a
>   function compiled on the host, calling, and releasing handles;
> - **events**: `PAUSED` 60, `RESUMED` 61, `SCRIPT_LOADED` 62. After `SCRIPT_LOADED`,
>   the device waits for `RESUME`: the host first sets the stops of the module;
> - **values**: primitives in direct CBOR, objects behind grouped handles (group
>   `PAUSE` released on resume, `CONSOLE`, `PERSISTENT`). Objects are described without
>   running JS;
> - **maximum size of a message**: 64 KB (functions compiled by the proxy). The C++ CBOR
>   now reads and writes floats and `undefined`;
> - **threads**: requests arrive on the transport thread and run on the
>   JS thread:
>   - while paused, in the pause loop;
>   - when idle, through a message of the event loop;
>   - while JS is running, from the interrupt handler, and only the
>     requests that run no JS (`PAUSE`, `SET_STOPS`…);
> - **tested** by `tests/debugger.test.js` through the desktop agent (52 checks:
>   breakpoints, step into / over / out, pausing an infinite loop, variables and closures
>   modified, calling functions compiled on the host, exceptions, `debugger;`);
> - **not done yet**: the "uncaught" prediction (the `UNCAUGHT` mode does not
>   pause), `[[Entries]]` of Map/Set, `--wait-debugger`, conditional breakpoints.

The protocol between the stub and the proxy is called "device agent": it serves debug **and**
deployment.

#### Framing

On every link (serial and TCP), a frame has the form:

`0x00 | COBS(magic | version | CRC-16 | message) | 0x00`

- COBS guarantees that there is no `0x00` byte inside the frame; `0x00` delimits the frames.
- A frame whose CRC is wrong is ignored.
- On a serial link, bytes that do not decode as a valid frame are console text.
  During a session, the agent sends the console of the program (`console.*`, uncaught
  exceptions) as `LOG` messages. The logs of ESP-IDF itself are not wrapped: on UART0 they
  stay plain text between the frames. Wrapping them is planned for the links that are not
  the console.

The magic marker is one byte, `0xE5`; the CRC is CRC-16/CCITT-FALSE (status block above).

#### Encoding

A hand-written subset of **CBOR**: integers, floats, strings, bytes, arrays and
integer-keyed maps.

#### Objects, handles and groups

- Objects stay on the target, behind **handles**, grouped like
  CDP object groups:
  - `pause`: released on resume;
  - `console`;
  - `persistent`.
- Primitive values travel inline.
- Names never travel: they come from the debug sidecar.
- `RELEASE` releases handles.

#### Robustness

- Requests are numbered: a duplicate gets the same response again, without being
  executed again.
- The agent gives the maximum size of a message in its answer to `HELLO` (`MAX_MESSAGE`,
  64 KB).
- Big payloads are chunked (`UPLOAD_BEGIN` / `UPLOAD_CHUNK` / `UPLOAD_END`).

#### Messages

| Category | Messages |
|---|---|
| Opening | `HELLO` (protocol version, token; the answer gives the maximum size), `INFO` |
| Deployment | `UPLOAD_BEGIN`, `UPLOAD_CHUNK`, `UPLOAD_END` (write a `.qjspkg` to flash), `RUN`, `STOP`, `RESET` |
| Debug session | `DEBUG_ATTACH`, `DEBUG_DETACH` |
| Execution control | `PAUSE`, `RESUME`, `STEP`, `SET_STOPS`, `SET_EXCEPTIONS` |
| Scripts | `LIST_SCRIPTS` |
| Inspection | `GET_STACK`, `GET_SLOTS`, `GET_PROPERTIES` (without running getters; it also gives the internal slots) |
| Modification | `SET_SLOT` |
| Code execution | `LOAD_FUNCTION`, `CALL` |
| Handles | `RELEASE` |
| Events (agent → host) | `LOG`, `STARTED`, `EXITED`, `PAUSED`, `RESUMED`, `SCRIPT_LOADED` |

The numbers and the fields of each message are in `host/js/protocol.js` and
`common/agent/protocol.h`, which must stay identical. Not done: listing the files of a device.

#### Security over TCP

Design: over TCP, the agent is **disabled by default**, and a **token** is required in
`HELLO`. Done today: the token (the desktop agent started with `--token`). No device has a
TCP link yet.

### 4.4 Position kinds

> **Status: done on 2026-11-10** (engine change E; the kinds are the numbers of the
> `positions` of the debug sidecar, section 4.2).

Each position recorded by the compiler carries a kind:

| Kind | Number | Origin |
|---|---|---|
| expression | 0 | the positions QuickJS already recorded (its pc2line table): error positions, stack traces |
| statement | 1 | statement start, `catch` clause, entry of the function body, loop condition and update: where the debugger can stop |
| return | 2 | each `return` and the implicit return (lets the return value be shown) |

### 4.5 The `.qjslib` libraries

> **Status: done on 2026-11-11** (`qjsx mklib`, `host/js/library.js`, `compile.js`,
> `build.js`; engine change J). Tested by `run-tests.sh` (fixtures `testlib`,
> `uselib`, `dynamicimport`, `nolibrary`) and `tests/tools.test.js`. Not done: checking
> the native modules against the firmware, the `.d.ts`.

**The problem.** A device has no compiler: a module written in JS and embedded as
source in the firmware (this was the case of `path`) cannot load there. And a generic
firmware must not grow with JS that most programs do not use.

**The answer.** The firmware contains no JS, only native modules. Reusable JS
modules live in libraries, `.qjslib` files. `qjsx build` copies into
the program's package the library modules it imports; the device finds them there
like any other module, knowing nothing about libraries.

**Names.** Every module of a library is called `<library>/<module>`, and is imported
under that name, on the model of `node:path`:

```js
import { join } from "stdlib/path";
import { read } from "sensors/bme280";
```

- The standard library is called `stdlib` (`stdlib.qjslib`).
- Native modules keep their short names (`fs`, `crypto`…).
- Two libraries cannot have the same name, so their modules never
  collide.
- Inside a library, a module imports its neighbours by their full name or relatively:
  `"./filter"` from `sensors/bme280` means `sensors/filter`.

**The `.qjslib` file.** A zip with stored entries, laid out like a `.qjspkg`:

| Entry | Content |
|---|---|
| `manifest.json` | `format` (1), `kind` (`"library"`), `name`, `version`, `created`, `compiler`, `bytecode`, `natives[]`, `modules[]` |
| `bytecode/<module>.qbc` | bytecode of the module: a cache, used only by a build whose compiler is the one that produced it |
| `debug/<module>.json` | its debug sidecar (same condition) |
| `src/<module>.js` | its source: what a build made with another compiler recompiles, and what the debugger shows |

Each item of `modules[]` gives `name` (`"sensors/bme280"`), `buildId`, `sourceHash`, the
names of its three entries and `imports` (the names of the modules it imports).

**Making a library: `qjsx mklib [directory] [--output <file>]`.** The command serves
`stdlib` as well as a user's libraries. The folder contains a library
`qjsx.json` (section 6.4): `name`, `version` and `modules`, which maps each
module name to its source file. It compiles each module under its full name, checks
that an import to the library itself does name one of its modules, notes the native
modules imported, and writes `<output>/<name>.qjslib`.

**The standard library.** Its sources are in `lib/stdlib/` (today `path`). The
build of `qjsx` makes `stdlib.qjslib` next to the executable, with the tool it has just
linked; this file is what is shipped, not the sources. The tool finds it in the folder of
its executable (`process.argv[0]` is the full path of the executable, as in Node).

The same sources are also embedded in the host executable, under the same names
(`stdlib/path`): the tool needs them for its own commands, `mklib` among them, before
`stdlib.qjslib` exists, and `qjsx run script.js` uses them for a script started without a build.

**What `qjsx build` does.**

1. It compiles the modules of the program (the files of `sources`).
2. It reads the libraries: `stdlib.qjslib`, then those of `libraries` in `qjsx.json`.
3. It follows the imports of each module. A name that is not a module of the program is:
   - a module of a known library: it is added to the package, whole, and its own
     imports are followed in turn;
   - a name without `/`: a native module, recorded in `natives` of the manifest;
   - otherwise an error, which names the importing module (relative import not found,
     library not declared, module missing from the library).
4. It writes into the package the bytecode, the debug sidecar and the source of each module, those
   of the libraries under `bytecode/<library>/<module>.qbc`. A library module
   that nobody imports is not copied; there is no finer sorting (function by
   function).

The debugger sees library modules like the others: their sidecar and their source
are in the package.

**Library made by another compiler.** The build compares the compiler
id and the bytecode number of the library (`compiler`, `bytecode` of its
manifest) to its own. If they differ, the library is not refused, since it contains
its sources: its modules are recompiled, and the build says so ("library 'testlib' (...) was
made by another compiler (...): its modules were compiled again from their sources").

**`import()` is forbidden** in a program built by `qjsx build`, and in a
library: the name of a module loaded by `import()` is known only at run time, so the
build could not take it along. The compiler refuses it as a syntax error, with
its position (engine change J). The JS of the tool itself is not concerned.

**Limits.**

- `qjsx run script.js` (without a build) only knows the embedded copy of `stdlib`: a script
  that imports another library must be built, then started with
  `qjsx run package.qjspkg`.
- A typo in the name of a native module (`"cryto"`) is only seen when the
  program is loaded: the build does not know the list of the native modules of a firmware. It is planned
  that the firmware gives it in `HELLO` and that `qjsx deploy` compares it to `natives`.
- If only `lib/stdlib/qjsx.json` changes, `make` does not remake `stdlib.qjslib` (it remakes it
  when the executable is relinked, so as soon as a source of `stdlib` changes).

---

## 5. The runtime

### 5.1 Event loop

> **Status: done** (`common/runtime/loop.h`, `loop.cpp`), for the host (`platform_host.cpp`,
> Windows and POSIX) and for the FreeRTOS devices (`ports/freertos/platform_freertos.cpp`);
> tested by `tests/runtime.test.js` and on the ESP32-S3. On the host, an uncaught exception
> stops the loop with code 1 (`setExitOnUncaught`), like Node; on a device it is printed and
> the program goes on. Not done: posting a message from an interrupt handler.

Same model on every platform (host and device):

- **a single JS thread**;
- **event producers** (drivers, I/O threads, later ISRs on the device) post messages
  into **a single bounded queue**, guarded by `platform::Signal`: a mutex and a condition on
  the host, a mutex and a binary semaphore on FreeRTOS (not a FreeRTOS queue, as first
  planned);
- the loop drains the promise jobs, waits for the next message or timer, then handles it on
  the JS thread;
- messages carry a **source id + a generation**, never a raw pointer
  to an object;
- the event loop of `quickjs-libc` is not used;
- the transport of the debug stub is one more producer; during a pause, a mini-loop
  only handles the debugger's messages.

An uncaught JS exception in a callback on the device is printed, then execution
continues; restarting remains an option.

### 5.2 Async styles

> **Status: rule applied** by the modules written so far (`net`, `serial`: callbacks for
> the data and the close, a promise for `connect`). The `uart` example below is the design:
> that module does not exist yet.

- **Callbacks** for spontaneous events: `uart.onData(fn)`.
- **Promises** for operations started by the script:
  `await uart.readLine({ timeout })`.
- A module may offer both.
- Async iteration (`for await`) may come later, built on promises.

### 5.3 C++ ↔ JS binding library

> **Status: done** in `common/jsbind/` (headers only) and tested by `tests/jsbind.test.js`
> with the native module `tests/native/test_module.cpp` (42 checks, no leak in a Debug
> build). It replaces the `uart`/`console` code of `legacy/devices/` (which had bugs,
> `console.cpp` did not compile). It answers the task "make it easier to write C objects
> visible in JS" of `TODO.md`. Not done: the generation of the `.d.ts`.
>
> Deviations from the design:
> - in a `void` function, C++ forbids `return <value>`: one writes
>   `return (void)js::fail(...);` (or `js::fail(...); return;`);
> - a module is declared by a function body: `JS_MODULE(fs, m) { JS_FUNCTION(m, readFileSync); JS_ADD_CLASS(m, Stats); }`,
>   the globals by `JS_GLOBALS(name, g) { ... }`; `JS_FUNCTION_AS(m, cppFunction, "jsName")`
>   when the C++ name clashes (for example `log` of `<math.h>`);
> - additional types: `js::Optional<T>`, `js::Args` (remaining arguments), `js::Created<T>` /
>   `js::create<T>()` to create from C++ an object of a bound class;
> - a pointer to a bound class (`Device *`) is accepted as an argument and returned as it is
>   (same JS object);
> - error names include the owner: `UART.open: argument 1, field 'speed': ...`,
>   `fs.readFileSync: ...`.

#### Constraints

C++17, header-only, **no `std::` at all**, no C++ exceptions, no RTTI.

#### Errors: "first error wins"

- Each call has an error slot; the first error recorded wins.
- `return js::fail(...)` is convertible to any return type.
- Variants: `js::fail<js::RangeError>()`, `js::failSystem()`.
- JS exceptions thrown in callbacks are captured automatically.
- It is the boundary between C++ and JS that throws the JS exception.
- System errors carry a Node-like `code` (`"ENOENT"`).

#### Constraints carried by types

- `js::Range<int, a, b>`: bounded integer.
- `JS_ENUM(Name, a, b)`: enumeration, passed as a string from JS.
- `JS_STRUCT(Options, fields...)`: options object, with C++ default values; unknown
  fields are rejected.

#### Own types

`js::Str`, `js::Bytes`, `js::Buffer`, `js::Function`, `js::Value`.

#### Classes and modules

- `JS_CLASS(Name)` inside the class, and a `JS_EXPORT(member)` marker before each exposed method or
  field. The mechanism relies on `__COUNTER__` and overloaded static registrars;
  slots 0 to 63 are scanned at compile time.
- The JS constructor corresponds to an optional `init(...)` method; the C++ constructor has
  no arguments.
- Fields become properties, read-only if they are `const`.
- `JS_MODULE(name, m) { ... }` declares a module: its body registers the functions
  (`JS_FUNCTION`) and the classes (`JS_ADD_CLASS`).

#### Lifetime and GC

- The destructor is only a **safety net**: it is called by the finalizer, and no
  JS code is allowed in it. Explicit closing goes through `close()`.
- `js::Value` members register automatically for GC marking (registered
  while the object is being constructed).
- `js::keepAlive(bool)` keeps an object alive as long as it has pending I/O (like Node).

#### Typings

`.d.ts` typings for VS Code can be generated from the declarations. Planned, not done:
`qjsx build` would write them (section 6).

### 5.4 Standard library

> **Status: done for what is listed here** (`common/runtime/modules/` for what is shared
> with the devices, `host/modules/` for the host only), tested by `tests/runtime.test.js`:
> - globals: `console` (`log`, `info`, `debug`, `warn`, `error`), `setTimeout`, `setInterval`,
>   `clearTimeout`, `clearInterval`, `TextEncoder` / `TextDecoder` (UTF-8 only), `process`
>   (`argv`, `env`, `platform`, `exit`, `cwd`), global as in Node; on a device (agent),
>   `argv` is `["qjsx", main module]`, `env` comes from the `env` section of qjsx.json (through
>   the manifest), `cwd()` is the data folder of the board (`board::dataDirectory()`,
>   `<storage>/data` for the desktop agent) and `fs` resolves relative paths against it;
>   `process.exit` ends the program, never the process (the agent survives);
> - `fs`: `readFileSync`, `writeFileSync`, `existsSync`, `statSync`, `readdirSync`,
>   `mkdirSync` (`recursive`), `rmSync` (`recursive`, `force`), `renameSync`, `unlinkSync`;
>   UTF-8 paths (wide APIs on Windows);
> - `stdlib/path`: written in JS, in the standard library (`lib/stdlib`, section 4.5);
>   semantics of Node's `path.posix`, plus drive letters and `\` as input on
>   Windows; results always with `/` (Node on Windows produces `\`);
> - `crypto`: `sha256(string or bytes)` → `Uint8Array`.
>
> - `net`: `connect({host, port})` → promise of a connection, `listen({host, port},
>   onConnection)` → server (`port()`, `close()`); connection: `onData`, `onClose`,
>   `write(text or bytes)`, `close()`, `remoteAddress()`, `remotePort()`. One read
>   thread per connection; `listen` listens on 127.0.0.1 by default (tested, `tests/net.test.js`);
> - `serial` (host): `list()`, `open(port, {baudRate})` → `onData`, `onClose`, `write`,
>   `close`, 8N1. Used on Windows with the ESP32-S3 board (deployment, debugger); the POSIX
>   part is written, not tried;
> - `process.stdout.write` / `process.stderr.write`.
>
> On the device, without a compiler, a module written in JS such as `stdlib/path` arrives as bytecode
> in the program's package (section 4.5).
>
> Infrastructure: the I/O threads post into a reference-counted "mailbox"
> (`rt::Mailbox`), which outlives the loop: a thread that posts after a runtime was
> destroyed has its message ignored. The loop is destroyed after `JS_FreeRuntime`
> (`Loop::shutdown()` frees its JS part before), because the finalizers remove their sources.

It replaces `quickjs-libc`, is written with the binding library, and is shared with
the device where it makes sense.

**Globals:**

- `console`;
- `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval`;
- `TextEncoder`, `TextDecoder`;
- `process`;
- `Uint8Array` for bytes (no Node-style `Buffer`).

**Modules:**

| Module | Platform | Description |
|---|---|---|
| `fs` | host; present on a device, where it fails with `ENOSYS` until a board has a file system | files, synchronous |
| `stdlib/path` | all (standard library, in JS) | paths |
| `crypto` | all | `sha256` |
| `net` | host | TCP, asynchronous |
| `serial` | host | serial port; on the device, the equivalent will be `uart` |
| `compiler` | host | compile API |

`process` is a global, on every platform. No hardware module (GPIO, I2C, `uart`…) exists yet.

**Naming rule:**

- when a function behaves exactly like Node's, it has Node's name
  (`fs.readFileSync`, `path.join`, `process.argv`…);
- otherwise, short names of the project's own, following the async rules of 5.2 (callbacks
  for events, promises for operations).

### 5.5 Device ports

> **Status: ESP32-S3 done on 2026-11-11** (ESP-IDF v6.1, 700 KB firmware, with no
> JS at all), flashed and tried on the YD board: agent over UART0, deployment and execution in place,
> debugger 52/52, program using `.qjslib` libraries.
> RP2350: designed, not implemented.

One firmware per board, generic: everything the chip can do, flashed once; the
programs then arrive as packages. Each port provides three things around the common code,
identical on every board (`agent::runDevice()`):

| Part | Role | ESP32 |
|---|---|---|
| `ports/cmake/qjsx.cmake` | common sources (engine without compiler, runtime, agent), engine options | shared with the RP2350 |
| `platform.h` | clock, fatal stop, tasks, locks, signal, periodic pause of the JS thread | `ports/freertos/` (locks, signal, `sleepMs`, `yieldIfDue`) + `platform_esp32.cpp` (`esp_timer`, `esp_system_abort`, pinned tasks) |
| `board.h` | identity, JS heap, JS stack, package storage, links, console | `board_esp32.cpp` |
| entry point | creates the JS task, which calls `agent::runDevice()` | `main.cpp` (48 KB task on core 1) |

**One description per board.** `ports/esp32` serves every ESP32 board; what changes from one
board to another is in `boards/<board>/`: `sdkconfig.board` (chip, flash size, table from
`partitions/`, PSRAM mode) and `board_config.h` (name, later links and pins). The
board is chosen at build time (`QJSX_BOARD`), each one in its `build-<board>` folder. The partition
tables go by flash size (4, 8, 16 MB), not by board.

**Long computation and watchdog** (decided, coded and verified on the YD board on 2026-11-11:
during the endless loop of `tests/fixtures/debuggee`, no "task_wdt" message in 17 s
of listening to the console, against one every 5 s before).

- *The problem.* On an RTOS, the JS task has a higher priority than the idle task.
  A program that computes without yielding to the event loop therefore starves the idle task of
  its core. Under ESP-IDF, the task watchdog reports it ("task_wdt ... IDLE1"
  every 5 s, without a restart), and the idle task no longer does its housekeeping (freeing
  deleted tasks).
- *The answer.* `platform::yieldIfDue()`, called by the interrupt handler of the runtime
  (`Runner::interrupt`, `common/agent/runner.cpp`), so regularly as long as JS
  is running. Every 100 ms (`YIELD_PERIOD_MS`, `ports/freertos/platform_freertos.cpp`),
  it blocks the JS task until the next tick (`vTaskDelay(1)`); the rest of the time it
  returns at once. Cost: 1% of the computing time at most with a 1 ms tick
  (`CONFIG_FREERTOS_HZ=1000`). On the PC, the function is empty.
- *What the watchdog still detects.* Its criterion becomes: "the JS thread has not
  gone through the engine's interrupt point for 5 s".
  - A script that loops (`while (true) {}`) goes through this point: the watchdog says
    nothing. This is intended: one cannot decide whether a long computation is intended or stuck, and the
    device stays controllable from the PC through this same point (debugger pause, stop,
    redeployment).
  - Native code that spins (busy wait in a driver, loop in a C module)
    no longer goes through it: the message appears, and neither pause nor stop is possible. This is
    a defect of the firmware, not of the script.
  - A deadlock (JS thread asleep on a lock) does not starve the idle task: it is not seen.
- *Rejected options.* No longer watching the idle task of core 1
  (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n`): the detection of native code that
  spins is lost. JS task at priority 0: it would be preempted by every other task.
- *Not handled.* A standalone device that recovers by itself from a stuck script: this would be a
  time budget chosen by the author of the program ("the event loop must get control
  back every N seconds"), suspended during a debugger pause. To be discussed.

**ESP32-S3** (YD board, WROOM-1 N16R8: 16 MB of flash, 8 MB of octal PSRAM):

- **Memory**: the JS heap is in PSRAM (`heap_caps_malloc`), the JS stack in internal RAM;
  `board::jsStackSize()` gives QuickJS a limit below the stack of the task (an overflow
  becomes a `RangeError`).
- **Partitions** (16 MB): two 3 MB firmware slots reserved for OTA (still to be
  decided), packages 2 MB, data 7.9 MB for a future `fs`. On 4 MB: no OTA.
- **Packages**: two slots, each one half of the partition. An upload goes into the one that is
  not installed; its header (size, sequence, build id) is written last. An interrupted upload
  therefore leaves the installed package intact, and the running program, mapped from its slot by
  `esp_partition_mmap`, is never erased under it.
- **Links**: the board starts all those it can handle; the host chooses its own, and
  the link that receives the `HELLO` carries the session. Today UART0 (COM port, CH343): the
  UART driver also serves the console (`uart_vfs`), one write per call, so a frame is
  never cut by text. To come: native USB, TCP over WiFi.
- **Console**: during a session, the program's output goes out as `LOG` messages on the link
  of the session; otherwise it goes to the ESP-IDF console (UART0, 921600 baud).
- **Engine**: two changes for newlib (`engine-changes.md`, I); compile options
  for GCC 14 and later on 32 bits (`int32_t` is a `long`).
- **Libraries**: a program that imports `stdlib/path` and a test library
  (`tests/fixtures/uselib`) gives on the board the same output as on the PC (2026-11-11);
  the modules come from its package (section 4.5).
- **Limits**: no file system yet (`fs` fails with `ENOSYS`, `cwd()` is `"/"`), no `net`,
  no hardware module.

How to add a board, or a new chip, is described step by step in
[new-device.md](new-device.md).

---

## 6. The host tool `qjsx`

> **Status: in progress.** Done (`host/`):
> - `qjsx run <file.js | package.qjspkg> [arguments]`: a script is run as an ES
>   module; a package is run from its bytecode, as the device does (package reader
>   `common/runtime/package.*`, without copying, shared with the device). Module loader: native,
>   bytecode of the package, embedded JS, files;
> - `qjsx build [directory]`: compiles the project (`qjsx.json`) into a `.qjspkg` (see 4.1), with
>   the library modules it imports (4.5);
> - `qjsx mklib [directory] [--output <file>]`: compiles a library project into a `.qjslib`
>   (4.5);
> - `qjsx deploy [package] --target T [--token X] [--no-run] [--monitor [--until-exit]]`,
>   `qjsx monitor`, `qjsx device info|run|stop|reset`, `qjsx targets`; the status messages
>   go to stderr, stdout only carries the program's output;
> - `qjsx agent [--host] [--port 5555] [--storage build/agent] [--token]`: the
>   device agent on the desktop (`host/agent_command.cpp`), which stands in for a board to
>   develop and test; same agent code as the firmware; installed program started at
>   startup; stopping a program even when it is stuck in a loop (interrupt handler);
>   an uncaught exception is printed and the program continues, as on the device;
> - `qjsx help`, `qjsx --version`.
>
> The commands other than `run` and `--version` are written in JS (`host/js/`), embedded
> in the executable as modules `qjsx:<name>` by a premake rule
> (`tools/embed-js.lua`). Native module `compiler` (host): `compile(source, name, { module,
> strip: "none" | "source" | "all", debug, dynamicImport })` → `{ bytecode, debug, imports,
> warnings }`, plus `quickjsVersion`, `qjsxVersion` and `bytecodeVersion`.
> Tests: `tests/run-tests.sh [Release|Debug]` (binding, runtime, tool modules, package
> run exactly like the sources, zip readable by an external tool, reproducible build,
> syntax error with its location). Build: `premake5 gmake --cc=gcc --shell=posix` then
> `make -C build config=release`; in `bin/Release/`: `qjsx`, `stdlib.qjslib`, `qjsx-tests`
> (which adds the test modules) and `qjsx-target` (the tool linked with the engine without
> compiler). Not implemented: `qjsx debug` and `qjsx trace`. The old program (`legacy/`) is
> no longer built; it stays on the author's disk and is not in the repository.

### 6.1 Composition

A single executable:

- host QuickJS, with the compiler;
- C modules: event loop, sockets, serial port, SHA-256, compile API;
- commands written in JS, embedded as source and compiled when the tool starts;
- next to the executable, the standard library `stdlib.qjslib` (section 4.5).

### 6.2 Commands

| Command | Role |
|---|---|
| `run` | runs a script on the PC, or a package from its bytecode like a device does |
| `build` | compiles the project and produces the `.qjspkg` (planned: the `.d.ts` of the native modules) |
| `mklib` | compiles a library project, produces the `.qjslib` (section 4.5) |
| `deploy` | sends a package to the device through the agent and starts it; `--monitor` then shows the program's output |
| `monitor` | shows the output of the program running on a device |
| `device info` / `run` / `stop` / `reset` | installed package and state, start, stop, restart of the device |
| `targets` | lists the serial ports of the PC |
| `agent` | the device agent on the desktop, which stands in for a board |
| `debug` | planned: starts the proxy, a CDP server for VS Code |
| `trace` | planned; its behaviour is not designed yet |

Planned too, not done: `device ls`, to list the files of a device.

### 6.3 Targets

A target names the link to a device:

| Example | Link |
|---|---|
| `serial:COM3` | Windows serial port |
| `serial:/dev/ttyUSB0@921600` | Unix serial port, with baud rate |
| `tcp:esp.local:5555` | TCP (WiFi or wired) |

### 6.4 Project file `qjsx.json`

At the root of the project (done, `host/js/project.js`). Keys, all optional; an unknown
key or one of the wrong type is an error:

| Key | Default | Role |
|---|---|---|
| `name` | name of the folder | name of the package (letters, digits, `.`, `_`, `-`) |
| `main` | `"src/main.js"` | module run at startup |
| `sources` | `["src/**/*.js"]` | modules compiled into the package (globs `*`, `?`, `**/`) |
| `exclude` | `[]` | patterns removed from `sources` |
| `output` | `"build/packages"` | folder of the packages |
| `keep` | `10` | number of packages kept for the project |
| `target` | — | default device of `deploy`, `monitor` and `device` when `--target` is not given |
| `env` | — | `process.env` of the program on a device (strings only) |
| `libraries` | `[]` | paths of the `.qjslib` files the program may import, in addition to `stdlib` |

A library project (`qjsx mklib`, section 4.5) replaces `main` and `sources` with:

| Key | Role |
|---|---|
| `name` | name of the library: its modules are imported under `<name>/<module>` |
| `version` | version of the library, mandatory |
| `modules` | object "module name → source file"; a name may contain `/` (`"drivers/bme280"`) |
| `output` | folder where `<name>.qjslib` is written |

### 6.5 VS Code integration

> **Status: the attach works through `qjsx debug` (3.9), not tried in VS Code itself yet.
> `.d.ts` and `--wait-debugger` not done.**

- "attach" configuration of type **`node`** (not `chrome`: js-debug answers the `Target.*`
  commands itself for a Node target), in the `.vscode/launch.json` of the project, with
  `qjsx debug` running in a terminal:

  ```json
  {
    "type": "node",
    "request": "attach",
    "name": "qjsx: attach",
    "address": "127.0.0.1",
    "port": 9339,
    "autoAttachChildProcesses": false,
    "attachExistingChildren": false,
    "skipFiles": [],
    "sourceMaps": false,
    "enableContentValidation": false
  }
  ```

- The scripts are announced with `file:///` URLs to the local sources.
- `qjsx build` writes the `.d.ts` of the native modules.
- The proxy keeps the breakpoints and re-applies them on `SCRIPT_LOADED` and after a device
  restart.
- `--wait-debugger`: see 3.8.

---

## 7. Open points and points to verify

### 7.1 To verify (flagged as such in the sources)

- Running the bytecode straight from flash with `JS_READ_OBJ_ROM_DATA`. Reading the package
  in place through `esp_partition_mmap` works; the engine still copies the bytecode it loads
  (section 4.1).
- js-debug target type for the "attach" configuration: node or chrome.
- The explanation of the microbench differences (inlining of `static` functions by gcc) is an
  unverified hypothesis.

### 7.2 Open tasks (`TODO.md`)

| Task | State |
|---|---|
| Analyze the possible optimizations of the engine (later), starting from the microbench differences (−48% to +27%) | open |
| Send JS sources (files, folders) to the target | done differently: `qjsx deploy` sends a `.qjspkg`, written to flash by the agent (4.3, 5.5) |
| Make it easier to write C objects visible in JS | done: binding library (5.3) |
| Decide what to remove from `quickjs-libc` | done: it is not used; the project's modules replace it (5.4) |

### 7.3 Points not designed yet

Most of the points this list held were settled when the code was written (framing, hashes,
keys of `qjsx.json` and of the manifest, commands, position kinds: see sections 4 and 6).
Still open:

- Writing back the variables modified by an evaluated expression (the wrapper function
  receives the variables as parameters; only `SET_SLOT` is described for changing a variable).
- Behaviour of the `trace` command.
- The reduced package sent to a device (which entries it keeps).
- The style of the JS API of the hardware modules.

### 7.4 Inconsistencies between sources

The inconsistencies noticed when this document was written have been fixed in the sources:
- breakpoint detection by two dispatch tables is indeed the decision taken;
- the device's bytecode is stripped of positions, sources and variable names, identical in
  debug and in production: all the debug info is in the host-side sidecar;
- no build for a device contains the compiler, even in debug ([quickjs-map.md](quickjs-map.md)
  has been updated).

---

## 8. Appendices

### 8.1 Related documents and tools

| Document | Content |
|---|---|
| [`CLAUDE.md`](../CLAUDE.md) | reference list of the decisions taken, repository layout, build |
| [`TODO.md`](../TODO.md) | tasks of the author of the project |
| [quickjs-map.md](quickjs-map.md) | map of `quickjs.c` (modules, dependencies, compiler couplings, debugger entry points) and description of the split version |
| [quickjs-items.csv](quickjs-items.csv) | every item of `quickjs.c`: module, kind, name, lines |
| [quickjs-split-perf.md](quickjs-split-perf.md) | original against split: tests, sizes, compile times, microbench |
| [tools/quickjs-split/README.md](../tools/quickjs-split/README.md) | generator of the split, regeneration, tests |
| [`tools/quickjs-split/split-config.js`](../tools/quickjs-split/split-config.js) | line ranges, exceptions, patches |
| [`tools/quickjs-split/split.js`](../tools/quickjs-split/split.js) | the generator |
| [`tools/quickjs-split/regen.sh`](../tools/quickjs-split/regen.sh) | regeneration of `engine/qjsx/` |

### 8.2 Repository layout

| Path | Content |
|---|---|
| `engine/qjs-bellard/` | upstream QuickJS 2026-06-04, unmodified |
| `engine/qjsx/` | the engine actually built: the same engine split into files by the generator, then modified by hand (2.2, 2.3) |
| `common/jsbind/` | the C++ ↔ JS binding library (5.3) |
| `common/runtime/` | event loop, platform interface, memory, files, package reader, SHA-256, modules shared with the devices (5.1, 5.4) |
| `common/board/` | `board.h`: what the common code needs from a board (5.5) |
| `common/agent/` | the device agent: protocol, deployment, debugger stub, runner of the installed package (4.3) |
| `host/` | the `qjsx` tool: C++ entry points, host-only modules, commands written in JS (`host/js/`), desktop platform and board (6) |
| `lib/stdlib/` | sources of the standard library (4.5) |
| `ports/` | device firmwares: `cmake/` (common build), `freertos/` (shared by the FreeRTOS ports), `esp32/` (5.5) |
| `tests/` | `run-tests.sh`, the test scripts and their fixtures |
| `tools/` | `quickjs-split/` (generator of the split engine, its test build and microbench), `embed-js.lua`, `idf.ps1`, style and documentation checks |
| `doc/` | documentation |
| `premake5.lua` | build of the desktop tool (2.8) |

### 8.3 Glossary

| Term | Meaning |
|---|---|
| Device agent | software embedded on the device that contains the debug stub and handles deployment |
| Debug sidecar | one JSON file per script holding the debug info, kept on the host |
| Build id | id of a script (SHA-256 of its bytecode, first 32 hexadecimal digits) or of a package (same, over the ids of its scripts, its main module and its `env`) |
| CDP | Chrome DevTools Protocol, spoken by js-debug |
| COBS | Consistent Overhead Byte Stuffing: an encoding that removes the `0x00` bytes from a frame |
| Proxy | CDP adapter on the host, between VS Code and the agent |
| Stub | minimal part of the debugger on the target, which exposes primitives |
| TDZ | temporal dead zone of a `let`/`const` variable before its initialization |
