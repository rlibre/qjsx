# Changes to the QuickJS engine

This document describes the changes made by hand in `engine/qjsx/`: first for the
debugger (A to F, L, M), then for the packages, the devices and the libraries (G to K). It
completes [architecture.md](architecture.md) (sections 2.6, 2.7, 3 and 4).

- Every change is commented `qjsx:` in the code.
- The exact diff against the unmodified split is
  [engine-changes.patch](engine-changes.patch). It is regenerated with
  `bash tools/quickjs-split/engine-diff.sh`.
- `engine/qjs-bellard/` (the original QuickJS) is not touched.
- `tools/quickjs-split/regen.sh` refuses to overwrite `engine/qjsx/` without `--force`, because
  that would erase these changes. To move to a new version of QuickJS: regenerate
  in another folder, then reapply `engine-changes.patch`.
- The bytecode format changes: `BC_VERSION` goes from 5 to `0x45`. A bytecode of another
  version is refused when it is read.

Changes A to M below are in the order in which they were made. The "Limits" section
lists what is not done or behaves differently from what was planned.

## A. Module name kept without the debug info

**Problem.** With `JS_STRIP_DEBUG`, QuickJS also throws away `debug.filename`. But `import.meta`,
`import()` and the backtraces find the current module by this name. The packages were therefore
compiled with `JS_STRIP_SOURCE` only, which kept the variable names and the pc2line
in the bytecode sent to the ESP.

**Change.**
- `JSFunctionBytecode.filename` moves out of the `debug` structure. It is always filled and always
  serialized.
- Files: `quickjs-internal.h`, `bytecode/reader.c`, `bytecode/writer.c`, `core/gc.c`,
  `core/errors.c`, `core/iteration.c`, `core/modules.c`, `compiler/finalize.c`,
  `compiler/funcdef.c`.
- `qjsx build` now compiles with `strip: "all"`: the bytecode of the package no longer contains
  any source, positions or variable names.

**Test.** `run-tests.sh`: "package runs like the sources", with a module that uses
`import.meta` and `import()`.

## B. Tail calls disabled

**Change.**
- `resolve_labels` no longer merges `call; return` into `OP_tail_call` nor
  `call_method; return` into `OP_tail_call_method`.
- File: `compiler/resolve-labels.c`.
- The `tail_call*` opcodes stay in the interpreter, but the compiler no longer produces them.

**Correction of a statement made during the design.** I had said that the tail call
of QuickJS removed the caller's frame, which would have truncated the call stacks. That is
wrong. QuickJS calls `JS_CallInternal` normally, then jumps to `done`: the caller's
frame stays on the stack during the call. The gain of the merge was limited to one dispatch.

Disabling them is still useful: the separate `OP_return` is where the debugger stops to
show the value of `return f()` (see E). It also follows the "always" decision
(same bytecode in debug and in production).

## C. `debugger;` → `OP_debugger`

**Change.**
- New opcode `OP_debugger` (1 byte), placed just before `nop` in `quickjs-opcode.h`. There are
  now 245 opcodes, for a limit of 256.
- The parser emits it for `debugger;` (`compiler/parser-stmt.c`). Before, the keyword was ignored.
- The interpreter calls the hook with the reason `JS_DEBUG_REASON_STATEMENT`. Without a hook, the
  opcode does nothing.

**Test.** `tests/debug.test.js`: section "debugger statements".

## D. Function number and lookup of a function

**Change.**
- `JSFunctionBytecode.function_number` (16 bits, serialized): number of the function in its
  script, in creation order. Child functions are created before their parent: the
  top-level function therefore gets the last number.
- The counter lives in the root `JSFunctionDef` (`function_counter`, in
  `compiler/compiler.h`). The number is given in `js_create_function`
  (`compiler/finalize.c`).
- `JS_DebugSetStops(ctx, module, number, ...)` finds the function by walking
  `ctx->loaded_modules`. For the module found, it goes down the tree of functions, which are
  stored in the constant pool (`core/debug.c`).
- `JS_DebugGetFunctionLocation(ctx, fn, &module, &number)` gives the reverse. The stub will
  need it for `[[FunctionLocation]]` in CDP.

**Deviation from the design.** There is neither a build id in the bytecode, nor a table of
functions built by the reader.
- The module name is now always present (A).
- The stub therefore identifies a script by the build id of the package (in the manifest), plus
  the name of the module.
- It identifies a function by the name of its module and its number.
- The lookup is done on demand, when the proxy sets stops. It is linear in the number
  of functions of the module, which is rare and cheap.
- Nothing has to be invalidated when a function is freed: the stops live in the function
  itself.

**Test.** `tests/debug.test.js`: `functionLocation`, then `setStops` on an unknown
function and on an unknown module.

## E. Positions and scopes for the debug sidecar

### Positions

QuickJS emits `OP_line_num <source offset>` at the places that can throw an exception, in
phase 1 (the parser). `resolve_labels` removes them and builds the pc2line table from them.

**Change.**
- The two high bits of the operand give a kind (`LINE_NUM_KIND_SHIFT`,
  `line_num_pos()` and `line_num_kind()` in `compiler/compiler.h`):
  - 0: expression (the original positions);
  - 1: statement start;
  - 2: return.
- `emit_debug_pos()` (`compiler/emitter.c`) emits a marker of kind 1 or 2.
- The markers are **always** emitted, with or without collection. The bytecode therefore does
  not depend on the option.

**Where the markers are emitted** (`compiler/parser-stmt.c`, `parser-func.c`, `parser-class.c`,
`parser-expr.c`):

- **Statement start (1)**:
  - at the start of `js_parse_statement_or_decl`, after the labels;
  - at the entry of a `catch` clause;
  - at the test of `while`, `do…while` and `for` (one stop per iteration);
  - at the increment expression of the `for`. This code is moved after the body of the loop, and
    its marker follows it.
  - An arrow function with an expression body also gets a marker at the start of the expression.
- **Return (2)**: just before the return opcode (`OP_return`, `OP_return_undef`,
  `OP_return_async`), so after the `finally` clauses.
  - For an explicit `return`, the position is that of the `return` keyword.
  - For the implicit return, it is the closing brace.
  - For an arrow function with an expression body, it is the last character of the body.
  - `emit_return` now receives this position. `NULL` means "no stop", for the
    generated returns: `return()` of generators and default constructors.

**Recording** (`compiler/resolve-labels.c`, `compiler/debug-info.c`):
- In phase 3, a marker of kind 1 or 2 is recorded at the pc of the next instruction
  emitted, that is at the current size of the output.
- The expression positions are recorded at the same place as the pc2line
  (`add_pc2line_info`), including when the debug info is stripped.
- Two stops at the same pc: the last one wins. For example `{` followed by its first statement.
- The kind masks are removed before the original pc2line.

**So that the optimizations do not break the stops:**
- The peephole optimizations (`code_match`) cross the markers, like the original
  positions. Example: `put_loc x; get_loc x` → `set_loc x`, straddling two statements.
- When a pass (`resolve_variables` or `resolve_labels`) skips several instructions at
  once because an optimization merged them, `js_debug_skipped_marker` looks for a marker
  in the skipped part:
  - in phase 2, it is emitted again;
  - in phase 3, it is recorded at the pc of the next emitted instruction.
  - The stop therefore falls just after the merged instruction. The absorbed part of the next
    statement has no effect (reading a local variable, for example).
- A skip that ends on a label is a dead code removal: its markers are
  dropped.
- `find_jump_target` stops on a marker. Otherwise, a jump to the implicit return would be
  replaced by a `return` without a stop.
- `skip_dead_code` forgets the markers of dead code: dead code has no stop.

**History.** A first version forbade `code_match` to cross a marker.
`global_read` of the microbench was then 60% slower, because of the `put_loc` /
`get_loc` merges lost between statements. Hence the mechanism above.

**Consequences to know:**
- If a merge absorbs the start of a `return x;`, its "statement" stop joins the
  "return" stop at the same pc, and only the latter remains. Stepping still stops on the line
  of the `return`.
- The markers update the "current position" of `resolve_labels`. The backtraces of
  scripts run from the sources (`qjsx run file.js`) can therefore give a column
  different from the original, in general more accurate.

### Scopes

**Deviation from the design.** The design planned scope entry and exit markers
kept until phase 3, to get pc ranges. These markers would have had to
go through `resolve_variables`, which removes `enter_scope`/`leave_scope`, then survive
dead code elimination. That is fragile.

Instead, each scope keeps its **source range** `[start, end)`:
- `JSVarScope.source_start` and `source_end`, filled by `push_scope` and `pop_scope`;
- scope 0 covers the whole function.

The proxy knows the source position of the stop thanks to the table of positions. From it, it
deduces the scopes that contain it, then the visible variables. This is also what V8 does
(source ranges in ScopeInfo).

### Collection

API in `quickjs-debug.h`: `JS_SetDebugInfoCollector(rt, fn, opaque)`.
- `js_create_function` calls the collector function for each compiled function, children
  first.
- The call takes place before the variable names are stripped (`js_debug_info_collect`, in
  `compiler/debug-info.c`).
- The collector function receives:
  - the number, the name and the source range of the function, and the size of the bytecode;
  - the positions sorted by pc;
  - the arguments;
  - the local variables, with their scope and their flags (`LEXICAL`, `CONST`, `CAPTURED`);
  - the closure variables (`GLOBAL` flag for global references);
  - the scopes (parent and source range).

On the host side:
- `compile(source, name, { debug: true })` returns `{ bytecode, debug }`
  (`host/modules/compiler_module.cpp`, format described at the top of the file). Without `debug`,
  `debug` is `null`.
- `qjsx build` writes `functions` in `debug/<path>.json` (`SIDECAR_FORMAT` 2). The index in
  `functions` is the function number.

**Test.** `tests/debug.test.js` checks the positions of `add()`: three statement starts
and one return. It also checks the exact sequence of stops when stepping in a `for` loop:
statement, test, body, increment, …, `return`. All the pcs seen by the hook are found
in the sidecar.

## F. Interpreter: stops, hook, frames

Public API in `engine/qjsx/quickjs-debug.h`, implemented in `core/debug.c`. The state
is in `JSRuntime.debug` (`JSDebugState`, in `quickjs-internal.h`).

### Two dispatch tables (`core/interpreter.c`)

- A local variable `dispatch` points to the normal table or to `debug_dispatch_table`.
  All the entries of the latter lead to `debug_check`, which checks the stops then jumps
  through the normal table.
- A frame takes the debug table only if the three conditions are met
  (`js_debug_frame_has_stops`):
  - a hook is installed;
  - the hook is not running;
  - the function has breakpoints, or statement starts while stepping
    is active.
- The rest of the code pays nothing.
- The choice of table is made again (`REFRESH_DEBUG_DISPATCH`):
  - at `restart`: entry, generator resume, jump to a `catch`;
  - after `OP_call`, `OP_call_method` and `OP_call_constructor`;
  - after a call to the hook;
  - when `js_poll_interrupts` really calls the interrupt handler, about every
    10,000 iterations. This is how a pause request coming from the transport
    reaches a running loop.
- Without "computed goto", a test before each instruction replaces the second table.
- Per function, two bitmaps (one bit per byte of bytecode): `debug_breakpoints` and
  `debug_statements`. They are not serialized and are freed with the function. The bytecode
  can stay in ROM: the bitmaps are in the structure in RAM.

### Hook

- `JS_SetDebugHook(rt, hook, opaque)`.
- The hook receives a `JSDebugEvent`:
  - `reason`: `BREAKPOINT`, `STEP`, `STATEMENT` or `EXCEPTION`;
  - `exception`: the thrown value;
  - `at_return` and `return_value`: filled when the stop instruction is a return.
- During the hook:
  - no nested call;
  - the hook can call JS (evaluation).
- The hook returns -1 to throw the exception it has set. This is not tested yet.
- `JS_DebugSetStepping(rt, on)`: when stepping, the hook is called at each statement start
  of the functions that have some. The stub filters "over" and "out" by comparing the frames. A
  pause request is stepping turned on.
- `JS_DebugSetStops(ctx, module, number, BREAKPOINTS | STATEMENTS, offsets, count)` replaces
  the stops of a function.

### Frames and slots

- `JS_DebugGetFrame(ctx, level, &frame)`: level 0 = stopped frame. The structure gives:
  - `id`: activation number of the frame, unique and never reused (section L; it was the
    address of the `JSStackFrame` before);
  - `is_async`: frame of an async function (section L);
  - the module and the number of the function;
  - the `pc`;
  - the numbers of arguments, local variables and closure variables.
- `JS_DebugGetFrameFunction`: the function of the frame.
- `JS_DebugGetSlot` and `JS_DebugSetSlot`: argument, local variable or closure, by index. A
  variable in the temporal dead zone returns a value of tag `JS_TAG_UNINITIALIZED`.
- Meaning of the `pc`:
  - at level 0, it is the exact stop instruction;
  - for an exception, it is somewhere in the instruction that throws;
  - for levels > 0, it is just after the call instruction.
  - The proxy must therefore take the last position ≤ `pc - 1`, except at level 0 for a stop.

### Exceptions

`JS_DebugSetExceptionReporting(rt, on)` enables the reporting. The hook is called only once
per thrown value, in the frame that throws it:

- in the `exception:` label, after the backtrace is built;
- during the hook, the pending exception is set aside, then put back afterwards;
- the reported value is remembered (`reported_exception`, reference counted). The same object
  does not go through the hook again while it goes up the calling frames.
- The value is forgotten when:
  - a `catch` clause catches it;
  - C code takes it (`JS_GetException`).
- It is **not** forgotten when the handler is a "rethrow": `gosub finally; throw`, or
  simply `throw` when the `finally` is empty and the optimizer removed the `gosub`.
- A value rethrown in an async function by a rejected `await`, or in a generator by
  `throw()`, is not reported again.

**Tests** (`tests/debug.test.js`, 41 checks):
- breakpoint: frames, arguments, local variables, temporal dead zone, modification
  of a variable;
- stop on the return with the returned value;
- removal of the stops;
- sequence of stops when stepping;
- `debugger;`;
- exception caught, crossing an empty or non-empty `finally`, crossing an `await`;
- no call without a hook.

The native test module is `tests/native/debug_module.cpp` (`qjsx-tests` only).

### Helpers for the stub (added with the debugger stub)

All these helpers are in `quickjs-debug.h` and `core/debug.c`:

- `JS_DebugGetObjectInfo(ctx, obj, &info)`: kind of the object (array, function, Map, Proxy,
  error…), name of its class in the engine, size (Map/Set, typed array, ArrayBuffer). Nothing
  runs JS code: no getter, no Proxy trap.
- `JS_DebugGetInternal(ctx, obj, which)`: the internal slots. They hold the target and the
  handler of a Proxy, the primitive value of a `Number`/`String`… object or of a `Date`, and the
  target of a function bound by `bind`.
- `JS_DebugClearAllStops(ctx)`: removes all the stops when the debugger detaches.

## G. Compiling a module without loading its imports

**Problem.** With `JS_EVAL_FLAG_COMPILE_ONLY`, QuickJS still resolves the imports of a module,
and therefore loads them. `qjsx build` failed ("could not load module") as soon as a module was
compiled before the ones it imports. The demo passed only because the alphabetical order of
its files compiled the dependencies first. The bug was found with the fixture of the stub
test (`main.js` imports `shapes.js`).

**Change.**
- New public flag `JS_EVAL_FLAG_NO_RESOLVE` (`quickjs.h`). `compiler/eval.c` then skips
  `js_resolve_module`. The original comment did in fact plan for this case: "Could add a flag
  to avoid resolution if necessary".
- The host module `compiler` uses it for modules.
- The bytecode produced does not change. The imports are resolved when the module is loaded to
  be executed.

**Test.** `tests/fixtures/debuggee` builds, in `tests/debugger.test.js`.

## H. Engine without compiler (targets)

**Change.** In `core/runtime.c`, `JS_NewContext` no longer calls `JS_AddIntrinsicEval`
when `QJSX_NO_COMPILER` is defined. Without this function, the whole `compiler/` folder is
useless. Outside `compiler/`, the engine has only one dependency on the compiler,
`JS_AddIntrinsicEval`; this is checked by cross-referencing the symbols of the object files. On
an engine without compiler, `eval()`, `Function()` and `JS_Eval()` throw
"eval is not supported".

**Build.** The premake project `qjsx-engine-target` is the engine without `compiler/`, with
`QJSX_NO_COMPILER`. The executable `qjsx-target` is the tool linked with this engine, without its
`compiler` module (`QJSX_NO_COMPILER`). It runs the packages as a target will.

**Test** (`run-tests.sh`). The fixture `tests/fixtures/nocompiler` covers classes and private
fields, generators, async, Map/Set, RegExp, JSON, typed arrays, BigInt and exceptions. It
gives the same output with `qjsx` and with `qjsx-target`, except `eval`, which fails on the target.

**Done since for the targets.** The JS modules that were embedded as source (`path`) could not
load on a target. They now live in libraries and travel as bytecode in the package of the
program (section J; `architecture.md`, 4.5): a firmware embeds no JS.

## I. Building with newlib (ESP32, RP2350)

Two changes, each with the user's agreement (2026-11-11, ESP32 port). Nothing
changes on the PC: Linux, macOS and Windows do not define `__NEWLIB__`.

**`malloc_usable_size`.** In `core/runtime.c`, the default allocator of QuickJS
(`js_def_malloc_usable_size`) calls `malloc_usable_size`, which newlib does not declare. A
`#elif defined(__NEWLIB__)` branch returns 0, as the original comment suggests. The
devices provide their own allocator (`board::jsAllocator`, in PSRAM on the ESP32): this
default allocator is not used there.

**Time zone without `tm_gmtoff`.** In `builtins/date.c`,
`getTimezoneOffset` reads `tm.tm_gmtoff` outside Windows, a field missing from newlib, the C
library of the ESP32 and of the RP2350. The condition `#if defined(_WIN32)` becomes
`#if defined(_WIN32) || defined(__NEWLIB__)`: newlib takes the Windows path (`gmtime`,
`localtime`, `mktime`), standard C. Nothing changes on the PC (Windows already took this path,
Linux and macOS do not define `__NEWLIB__`).

## J. Imports of a module and refusal of `import()` (`.qjslib` libraries)

Two changes, with the user's agreement (2026-11-11), for `qjsx build` and
`qjsx mklib`: a program carries in its package the library modules it imports,
so the build must know all its imports (see `architecture.md`, 4.5).

**List of the imports of a compiled module.** QuickJS gave no public way to read the
modules that a module requests.
- Two public functions in `quickjs.h`, written in `core/modules.c`:
  `JS_GetModuleRequestCount(m)` and `JS_GetModuleRequestName(ctx, m, index)` (an atom to
  free, `JS_ATOM_NULL` out of bounds).
- The names are those of the source, not normalized, in the order of the source. A module
  imported by several statements comes back several times: the caller removes the duplicates.
- The host module `compiler` returns them in `imports`; `host/js/compile.js` normalizes them as
  the engine does at load time (`js_default_module_normalize_name`).

**Refusal of `import()`.** An `import()` loads a module whose name is known only at
run time: the build could not carry it.
- New public flag `JS_EVAL_FLAG_NO_DYNAMIC_IMPORT` (`quickjs.h`). `compiler/eval.c` copies
  it into a new field `no_dynamic_import` of `JSParseState` (`compiler/compiler.h`).
- In `compiler/parser-expr.c`, `import(` then becomes a syntax error
  ("'import()' is not allowed: use a static import"), reported at the position of the call.
  `import.meta` is not touched.
- The host module `compiler` passes the flag when its option `dynamicImport` is `false`
  (`qjsx build` and `qjsx mklib` always do; the tool's JS is not concerned).
- Without the flag, nothing changes: the bytecode produced is the same.

**Tests.** `tests/tools.test.js` (list of the imports, refusal and reported line);
`run-tests.sh` ("import() refused with its position", fixture
`tests/fixtures/dynamicimport`). The original QuickJS tests pass with the modified engine.

## K. Public bytecode number and compile warnings

Two changes, with the user's agreement (2026-11-11).

**Bytecode number.** An engine only reads the bytecode of its own version (`BC_VERSION`,
checked by `JS_ReadObject`). To refuse a package compiled for another engine before
installing or loading it, this number must be known from the outside.
- New public function `JS_GetBytecodeVersion()` (`quickjs.h`, written in
  `bytecode/reader.c`).
- `qjsx build` writes it in the manifest of the package (`bytecode`); the agent refuses an
  upload of another number, and the loader refuses to run such a package (`architecture.md`, 4.1).
- **Rule: any change to the engine that modifies the bytecode (opcodes, serialization)
  increases `BC_VERSION`.** Without that, a package from before the change would be accepted and
  read wrongly.

**Compile warnings.** On a device, without compiler, `eval()` and the
`Function` constructor throw "eval is not supported"; on the PC they work. The
compiler therefore reports these uses when it compiles a source.
- API in `quickjs-debug.h`: `JS_SetCompileWarningHandler(rt, handler, opaque)`; the handler
  receives the kind (`JS_COMPILE_WARNING_EVAL`, `JS_COMPILE_WARNING_FUNCTION`), the line and the
  column (starting at 1, as in a `SyntaxError`). Two more fields in
  `JSDebugState` (`quickjs-internal.h`).
- `compiler/debug-info.c`: `js_compile_warning()` calls the handler, and
  `js_last_op_is_scope_var()` tells whether the last instruction emitted reads a variable by its
  name.
- `compiler/parser-expr.c` reports: the direct call `eval(...)` (where it chooses `OP_eval`),
  the call `Function(...)`, and `new Function`, with or without arguments.
- The detection is syntactic. An indirect use (`(0, eval)(x)`, `globalThis.eval`, an
  alias) is not seen; a variable of the program that bears one of these names is reported too.
- Without a handler, nothing changes: the bytecode produced is the same.

**Tests.** `tests/tools.test.js` (kinds and positions); `run-tests.sh` (warning at the
build of `tests/fixtures/nocompiler`, package of another number refused by the agent and by the
loader). The original QuickJS tests pass with the modified engine. The microbench did not
give a usable measurement that day (too much noise between two passes of the same binary)
and was not run again; these changes do not touch the interpreter.

## L. Activation number of the frames, async frames seen as bytecode

Debugger work, choice of the user (2026-11-11). Stepping "over" and "out" compared the
frames by the address of their `JSStackFrame` and by the depth of the stack. Two calls made one
after the other from the same place have the same address (`second()` after `first()` in
`add(first(), second())`), and a callback of the event loop that runs during an `await` has a
shallow stack: the step stopped in unrelated code.

**Activation number.**
- New field `JSStackFrame.debug_activation` (`quickjs-internal.h`), set to 0 when the frame is
  created: in `JS_CallInternal` (`core/interpreter.c`) and in `js_call_c_function`
  (`core/closures.c`). This store is the only cost on the call path. The frame of a generator or
  an async function lives in its state, zeroed by `async_func_init` (`memset`), and keeps its
  number across `yield` / `await`.
- `JS_DebugGetFrame` gives a frame its number the first time it is asked for it
  (`JSDebugState.last_activation`, 64 bits: never reused). `JSDebugFrame.id` is now this number
  (`uint64_t`), no longer an address.
- New field `JSDebugFrame.is_async`: the function is async (async function or async
  generator, `func_kind & JS_FUNC_ASYNC`): an `await` suspends it, a promise job resumes it with
  other callers.
- What the stub does with it: [architecture.md](architecture.md), stepping of the debugger stub.

**Async frames seen as bytecode (fix).** `core/debug.c` recognized a function running bytecode
only by the class `JS_CLASS_BYTECODE_FUNCTION`. The frames of async functions, generators and
async generators (classes `JS_CLASS_ASYNC_FUNCTION`...) were therefore described as native: no
module, no function number, no pc, no slots. `js_debug_frame_bytecode`,
`JS_DebugGetFunctionLocation` and `js_debug_module_bytecode` (a module with a top-level `await`
is an async function) now use `js_class_has_bytecode()`, as the backtraces do.

**Bytecode.** Unchanged: `BC_VERSION` stays `0x45`.

**Tests.** `tests/debugger.test.js` (72 checks), fixture `tests/fixtures/debuggee`: step over
the end of `first()` reaches the next statement of the module, not `second()`; step over an
`await` while a timer fires goes on in the same async function; step out of an async function
resumed by a promise job stops in the next code run. Final test of 2026-11-11: Release and
Debug, the original QuickJS tests, and on the ESP32-S3 (72/72). Microbench (3 passes per
version, totals stable within 0.2 %): −3.3 % against the original, `func_call` −8.5 %: the
store added on the call path costs nothing measurable (table:
`C:\temp\qjs-engine-test\bench-table-activation.md`).

## M. Prediction of uncaught exceptions

Debugger work, choice of the user (2026-11-11), instead of the `sf->call_sp` of the
original design (a store before every call, and before every property access that may
call a getter). The pause mode "uncaught" needs to know, at the throw, whether a handler
will catch the value.

- New field `JSDebugEvent.uncaught`, filled for `JS_DEBUG_REASON_EXCEPTION` by
  `js_debug_is_uncaught()` (`core/debug.c`). `js_debug_on_exception()` now receives the
  value stack of the throwing frame (`sp`, `stack_buf`), from the `exception:` label of
  `JS_CallInternal`.
- The prediction, only computed when a value is reported to a hook (nothing on the hot
  path):
  1. the throwing frame: a catch offset on its real stack, except offset 0 (iterator
     close) and a rethrow handler (`gosub; throw` or `throw`: a `finally`,
     `js_debug_is_rethrow_handler`);
  2. an async function or async generator catches everything (the value becomes a
     rejection, `js_debug_rejects`), except the top-level function of a module
     (`<eval>`): QuickJS compiles every module as an async function, but its rejection is
     the failure of the module;
  3. each calling frame: its pc (just after its call) inside the try region of a
     `catch` clause, found statically by reading its instructions
     (`js_debug_pc_in_try`: from an `OP_catch` to its handler, the handler not being a
     rethrow), or an async function as in 2; native frames let the value through;
  4. nothing down to the bottom of the stack: uncaught.
- Known approximation: a throw in a promise callback reaches the bottom of the stack and
  is predicted uncaught, although it becomes a rejection.
- **Bytecode**: unchanged. **Tests**: `tests/debug.test.js` (caught by the caller,
  through a `finally`, through a native call, in an async function, nothing to the bottom
  of the stack), and the "uncaught" mode of the stub in `tests/proxy.test.js` (a caught
  exception does not pause).

## Limits and items not done

- **"Uncaught" prediction**: done statically (section M), not with the planned
  `sf->call_sp`. A throw in a promise callback (`then`) is predicted uncaught although it
  becomes a rejection.
- **Refresh of the table.** A frame that called a getter, a `valueOf` or a
  native callback only changes table at its next call, backward jump or return. A "step
  out" from a getter can therefore stop a little further than expected.
- **`this`** is visible only if the function uses `this`: QuickJS then creates a local
  variable `this` only in that case. Elsewhere, the debugger cannot display it.
- **Only modules are found** by `JS_DebugSetStops`. The packages contain only
  modules.
- **Stepping.** The "over" / "out" filtering is in the stub: a "step over" of a long
  call calls the hook at each statement of the callee, if the callee has its statement starts
  set. A filter by frame depth in the engine will come if it is too slow.
- **Identity of exceptions.** Two cases where an exception is not reported:
  - `throw` of a primitive value identical to the previous one not yet forgotten;
  - a `finally` that swallows the exception with `return`, then the same value rethrown later.
- **`return` of the hook at -1**: not tested.
- **Warnings.**
  - In Release, `array.c`, `interpreter.c`, `properties.c` and `runtime.c` give
    "unused variable" warnings. They also exist in the unmodified split: they are
    variables that are only used in `assert`s, removed by `NDEBUG`. They do not come
    from these changes.
  - `-Wmaybe-uninitialized` in `common/jsbind/jsbind_class.h:192`, for a `bool` argument (seen
    when compiling `debug_module.cpp`). Probable false positive, not fixed.

## Performance

Measurements of 2026-11-10:
- Bellard's `tests/microbench.js`, 3 passes per version, same method as
  [quickjs-split-perf.md](quickjs-split-perf.md);
- `qjs` built with the original engine and with the split and modified engine;
- no debugger attached;
- the times are ns per operation: a negative difference means faster than the original.

| Version of the split | Original total | Split total | Difference |
|---|---:|---:|---:|
| split only (before these changes, quickjs-split-perf.md) | — | — | −3.8% |
| + A, B, C and two dispatch tables | 4,645.6 | 4,455.6 | −4.1% |
| + positions (E), final version | 4,665.4 | 4,546.3 | −2.6% |
| + activation number of the frames (L), 2026-11-11 | 4,637.7 | 4,485.8 | −3.3% |

- The final version costs about 2% more than the version without markers. Part of this
  difference stays within the noise from one pass to the next: `regexp_replace`, purely in C,
  varies by ±10%.
- Bellard's tests (`test_closure`, `test_language`, `test_builtin`, `test_loop`,
  `test_bigint`, `test_cyclic_import`, `test_worker`) pass with the modified engine.
- Full tables: `C:\temp\qjs-engine-test\bench-table-dispatch.md` and
  `bench-table-final.md`.

## Files touched

| File | Changes |
|---|---|
| `quickjs-debug.h` (new) | public API of the debugger and of the collection |
| `quickjs-internal.h` | `filename`, `function_number`, bitmaps in `JSFunctionBytecode`; `JSDebugState`; `js_debug_frame_has_stops`; `BC_VERSION` |
| `quickjs-opcode.h` | `OP_debugger` |
| `quickjs.h` | `JS_EVAL_FLAG_NO_RESOLVE` (G); `JS_EVAL_FLAG_NO_DYNAMIC_IMPORT`, `JS_GetModuleRequestCount`, `JS_GetModuleRequestName` (J); `JS_GetBytecodeVersion` (K) |
| `bytecode/reader.c` (also) | `JS_GetBytecodeVersion` (K) |
| `quickjs-debug.h`, `quickjs-internal.h`, `compiler/debug-info.c`, `compiler/parser-expr.c` (also) | compile warnings (K) |
| `compiler/eval.c` | no resolution of the imports with `JS_EVAL_FLAG_NO_RESOLVE` (G); `no_dynamic_import` (J) |
| `compiler/parser-expr.c` (also) | refusal of `import()` (J) |
| `core/modules.c` (also) | reading the imports of a module (J) |
| `core/debug.c` (new) | hook, stops, frames, slots, exceptions |
| `core/core.h` | `js_debug_*` prototypes |
| `core/interpreter.c` | two tables, `debug_check`, `OP_debugger`, refreshes, exception reporting |
| `core/errors.c` | `filename`; `JS_GetException` forgets the reported exception |
| `core/runtime.c` | initialization and release of `reported_exception`; `JS_NewContext` without eval with `QJSX_NO_COMPILER` (H) |
| `core/gc.c`, `core/iteration.c`, `core/modules.c` | `filename` outside debug; release of the bitmaps |
| `bytecode/reader.c`, `bytecode/writer.c` | `filename` and `function_number` serialized |
| `compiler/compiler.h` | scopes with source range, kind of the `OP_line_num`, collection fields |
| `compiler/debug-info.c` (new) | recording of the positions, collection |
| `compiler/emitter.c` | `emit_debug_pos`, ranges of the scopes |
| `compiler/parser-*.c` | position markers, `OP_debugger`, `emit_return` with position, end of functions |
| `compiler/resolve-labels.c` | tail calls, recording, `code_match` / `find_jump_target` / `skip_dead_code` |
| `compiler/finalize.c` | function number, call of the collection, `filename` |
| `compiler/funcdef.c` | scope 0, release of the positions |
| `builtins/date.c` | `getTimezoneOffset` without `tm_gmtoff` with newlib (I) |
| `core/runtime.c` (also) | `js_def_malloc_usable_size` returns 0 with newlib (I) |
| `quickjs-internal.h`, `quickjs-debug.h`, `core/debug.c`, `core/interpreter.c`, `core/closures.c` (also) | activation number of the frames, `is_async`, async frames seen as bytecode (L) |
| `quickjs-debug.h`, `core/debug.c`, `core/core.h`, `core/interpreter.c` (also) | prediction of uncaught exceptions, `JSDebugEvent.uncaught` (M) |
