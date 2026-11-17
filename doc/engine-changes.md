# Changes to the QuickJS engine

This document describes the changes made by hand in `engine/qjsx/`: first for the
debugger (A to F, L, M), then for the packages, the devices and the libraries (G to K),
then for the speed and the memory of the devices and for the tool (N to Y). It
completes [architecture.md](architecture.md) (sections 2.6, 2.7, 3 and 4).

- Every change is commented `qjsx:` in the code.
- The exact diff against the unmodified split is
  [engine-changes.patch](engine-changes.patch). It is regenerated with
  `bash tools/quickjs-split/engine-diff.sh`.
- `engine/qjs-bellard/` (the original QuickJS) is not touched.
- `tools/quickjs-split/regen.sh` refuses to overwrite `engine/qjsx/` without `--force`, because
  that would erase these changes. To move to a new version of QuickJS: regenerate
  in another folder, then reapply `engine-changes.patch`.
- The bytecode format changes: `BC_VERSION` goes from 5 to `0x45` (A, D), then to `0x46` (S).
  A bytecode of another version is refused when it is read.

Changes A to Y below are in the order in which they were made. The "Limits" section
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

## N. Temporal dead zone checks removed where useless

Speed work of 2026-11-12, agreed with the user. `resolve_variables()` turns every read
of a local `let` / `const` / `class` into `OP_get_loc_check` and every write into
`OP_put_loc_check`, which throw while the variable is in its temporal dead zone.
Nothing removed them (Bellard's own note in `resolve_labels()`: "XXX: should check if
variable can be used before initialization"), and the peephole optimizations of
`resolve_labels()` only match the plain opcodes (`i++` on a `let` did not become
`inc_loc`). On the PC, the same loop took 363 ms with `let` and 251 ms with `var`.

- New pass `remove_useless_tdz_checks()` (`compiler/tdz.c`), called by
  `js_create_function()` (`compiler/finalize.c`) between `resolve_variables()` and
  `resolve_labels()`: a forward data-flow analysis, "this local is surely initialized",
  over the bytecode whose labels are still symbolic, repeated until the states of the
  labels no longer change; `OP_enter_scope` puts the variables of its scope in the dead
  zone (function declarations excepted), an initialization takes them out, a label keeps
  what is initialized on every path reaching it; the handler of `OP_catch` gets the state
  at the `OP_catch`, the path goes on after `OP_gosub`.
- A check on a variable surely initialized becomes `OP_get_loc` / `OP_put_loc`, same size:
  nothing moves, and the optimizations of `resolve_labels()` then apply.
- Left as they are: closure variables, `this` of a derived class constructor, functions
  calling `eval()` directly, code the scan does not reach, functions too large for the
  analysis (more than 2^20 words of label states).
- **Bytecode**: no new opcode, the format is unchanged: `BC_VERSION` is not bumped
  (packages built before stay valid, they are only slower). Debug info is recorded after
  the pass, from the same positions.
- **Debugger positions** (found by `tests/debug.test.js` and `tests/debugger.test.js`
  once `let` / `const` compile like `var`): the peephole `put_x(n) get_x(n) -> set_x(n)`
  of `resolve_labels()` no longer crosses a statement or return position
  (`js_debug_has_stop_marker`, `compiler/debug-info.c`): `const r = f(); return r;` kept
  one stop for the start of `return r;` and its return, so a step stopped at the return
  too early. It was already the case for `var` code. The bytecode stays the same with or
  without debug info (the positions are always in the bytecode before this pass).
- **Gain** on the PC: the `let` loop 363 → 246 ms (`var`: 245 ms); the workload of
  `tools/profile/workload.js` (classes, closures, JSON, strings) does not change beyond the
  noise (1,009 to 1,034 ms, before and after): its time is elsewhere (calls, properties,
  allocations).
- **Tests**: `tests/tdz.test.js` (29 checks: the accesses before the declaration still
  throw, through blocks, `switch`, loops, labels, `try` / `catch` / `finally`, generators,
  closures, hoisted functions, destructuring defaults; the accesses after it read the right
  value).

## O. Seed of `Math.random()`

2026-11-13, agreed with the user. `js_random_init()` (`builtins/math.c`) seeds the
xorshift64* generator of `Math.random()` with `gettimeofday()` when the context is
created. On a board the clock is not set and starts at 0 at each boot: the seed is the
number of microseconds since the boot, nearly the same every time, and so is the
sequence.

- New public function `JS_SetRandomSeed(ctx, seed)` (`quickjs.h`, `builtins/math.c`):
  sets `ctx->random_state` (0 taken as 1, the state must not be 0).
- The runtime calls it for each program run (`createRuntime()`, `common/runtime/
  program.cpp`) with 8 bytes of `rt::randomBytes()`: the platform's source of
  randomness, or the reproducible sequence of a digital twin with a `randomSeed`.
- `Math.random()` stays a fast, non-cryptographic generator; `crypto.getRandomValues()`
  is the one for secrets.
- **Bytecode**: unchanged, `BC_VERSION` not bumped.

## P. Names of the built-in objects in read-only memory

2026-11-13, asked by the user (an ESP32-S3 without PSRAM had 5 KB of RAM left), TRIED
ON THE PC ONLY. A new context makes 515 atom indexes (`QJSX_ROM_ATOM_COUNT`): index 0
(`JS_ATOM_NULL`), the 242 predefined atoms and 272 names of built-in methods (`copyWithin`, `toLocaleDateString`...). Each one was a copy in the heap
of a text already in the firmware: on a 32-bit board about 5 KB of text, 6 KB of
`JSString` headers, 4 KB of block headers, plus the rounding.

- **Switch**: `QJSX_ROM_ATOMS` in `quickjs.h` (1 by default); 0 gives back the original
  QuickJS. A copy of the five files before the change is in `temp/rom-atoms-backup/`
  (not in git).
- **Immortal reference count**: `JS_REF_COUNT_IMMORTAL` (0x40000000). `JS_DupValue`,
  `JS_FreeValue` (and their `RT` versions) and `js_free_string` never change a count at
  or above it (`JS_REF_COUNT_IS_MORTAL`). This is the cost on the hot path (below).
- **The names**: `core/rom-atoms.h` and `core/rom-atoms.c`, GENERATED by
  `tools/rom-atoms/generate.sh` (builds `tools/rom-atoms/generate.c` with the engine of the
  devices as the original QuickJS, makes a context, writes the atoms it holds). Each name is
  laid out as QuickJS allocates an atom string (block header with the immortal count,
  `JSString`, characters), all in one `const` block (`qjsx_rom_atoms`, in flash on a
  device, read-only on the PC: a write would crash); the atom index is kept in
  `hash_next`. `qjsx_rom_atom_ptr` gives the string of each index (NULL for the 14
  predefined symbols, still made in the heap: the count of weak references of a symbol is
  written in its `hash`); `qjsx_rom_atom_hash` is an open addressing table by hash.
  Indexes beyond `JS_ATOM_END` follow the order of creation of the context. Run the
  script again when the built-in objects of the engine change.
- `core/atoms.c`: `__JS_AtomIsConst` covers the names in read-only memory (never
  counted); `JS_InitAtoms` puts them in the atom table (the table of the heap starts at
  128 free entries, its hash table at 128 slots, grown by the count of the heap's atoms
  only); `__JS_NewAtom` and `__JS_FindAtom` look in read-only memory first;
  `js_get_atom_index` reads the index of a name in read-only memory in `hash_next`;
  `JS_FreeAtomStruct` asserts it never gets one.
- `core/runtime.c`: `JS_FreeRuntime` does not free them; `core/gc.c`:
  `JS_ComputeMemoryUsage` does not count them.
- **Bytecode**: unchanged (it carries its atoms as text), `BC_VERSION` not bumped.
- **Measures on the PC** (64-bit, empty context of the engine of the devices):
  155.5 KB -> 133.0 KB of JS heap (-22.5 KB); with QuickJS's 4 KB arenas turned off
  (`JS_MALLOC_LARGE_BLOCKS_ONLY`), 91.4 KB -> 66.4 KB. Empty program on the desktop agent
  (`qjsx device info`): 175.0 KB -> 155.6 KB used, 196.2 KB -> 168.8 KB at most. Not
  measured on a board.
- **Speed**: microbench 4,450.7 -> 4,571.0 (+2.7 %, `QJSX_ROM_ATOMS` 0 then 1, same
  session, 3 passes each; tables `C:\temp\qjs-engine-test\bench-table-rom-atoms-off.md`
  and `bench-table-rom-atoms.md`). Some tests move by ±15 % both ways (code layout).
- Not done: the names of qjsx's own globals and native modules (`console`, `Gpio`...)
  are still made in the heap; a check that the generated files match the engine (they
  are made again by hand).

## Q. Arenas of the allocator tuned for a small heap

2026-11-16, agreed with the user. QuickJS 2026 has its own allocator over the host's
(`core/runtime.c`): blocks up to 512 bytes (header included) come from 4 KB arenas, one
arena per block size (31 sizes), given back to the host when their last block is freed;
larger blocks go to the host directly. Fast (a free list per arena), but each block size
in use holds at least 4 KB: an empty context had 28 arenas, 52 % empty; on the ESP32-S3
without PSRAM the program of a board used 255.8 KB of JS heap with them and 210.7 KB
without (`JS_MALLOC_LARGE_BLOCKS_ONLY`).

- Three compile options (`quickjs-internal.h`), set by the port of a board whose JS
  heap is small (`QJSX_SMALL_HEAP_DEFINES`, `ports/cmake/qjsx.cmake`; the ESP32 port gives
  them without PSRAM); unset, the original QuickJS:
  - `JS_MALLOC_ARENA_SIZE` (4096; small heap: 1024);
  - `JS_MALLOC_MAX_SMALL_SIZE` (512; small heap: 128): on the board, 97 % of the
    allocations of a program are at most 128 bytes, in steps of 8 that match the engine's
    structures; above, 3 % of the allocations and 44 % of the bytes: the host's heap;
  - `JS_MALLOC_ARENA_CACHE` (0; small heap: 4): empty arenas kept for the next request
    instead of going back to the host. Not 0: every arena is allocated
    `JS_MALLOC_ARENA_SIZE` bytes whatever its block size, so a kept arena serves any
    size (`js_malloc_new_arena`, `__js_free`). They go back to the host when a large
    allocation fails (`js_malloc_large`, then one more try) and in `JS_FreeRuntime`
    (`js_malloc_free_arena_cache`). With it also: an arena is made in constant time
    (`JSMallocArena.n_init_blocks`: the blocks never allocated are taken in order, the
    free list only holds freed blocks; QuickJS chains every block when it makes an
    arena), and a large block of at most 1 KB that grows is given a multiple of 32 bytes
    (`__js_realloc`: QuickJS's arenas gave that room up to 512 bytes).
- Three `_Static_assert`: the largest small size is at most 512 and fits in an arena;
  with `JS_MALLOC_ARENA_CACHE`, the block count of an arena fits in the byte
  `n_init_blocks`.
- The host under it, on a board without PSRAM: one region of internal RAM managed by
  `memory::RegionHeap` (see architecture.md, "Memory of a device"); not an engine change.
- **Measure option** `QJSX_ALLOC_HISTOGRAM` (2026-11-13, temporary: `idf.py
  -DQJSX_ALLOC_HISTOGRAM=ON`): every block goes to the host and is counted by size
  (`JSAllocHistogram`, `JS_GetAllocHistogram()`, the global `__allocHistogram()` of
  globals.cpp). It gave the figures above. A second measure option, `QJSX_MEMORY_MAP`
  (temporary too), adds `JS_DumpMallocMap(rt)` (`quickjs.h`, `core/runtime.c`): the
  arenas by block size, the arenas kept aside and the bytes of the large blocks (the
  global `__memoryMap()` of globals.cpp).
- **Measures on the board** (ESP32-S3 without PSRAM, 2026-11-16, with the region of
  architecture.md): its program 255.8 KB -> 205.8 KB of JS heap; an empty program
  62.0 KB (157.1 KB at the start of this work, with the names in the heap).
- **Speed**: on the PC, microbench +3.9 % with the small arenas over the system malloc
  (string building +50 %; the same loops over a RegionHeap are faster than over
  malloc). On the board a throw-away benchmark (temp/allocbench) against the shared
  heap with QuickJS's arenas: objects -9 %, arrays equal, JSON +8 %, properties +27 %,
  string building +62 %, closures in a loop +85 %; the two additions above (arena in
  constant time, room when growing) changed nothing. The cause is not found: the same
  loops are not slower on the PC. The instruction cache of the firmware is 16 KB for a
  25 KB `JS_CallInternal`: a closure made in a loop costs 26 to 52 us against 10 us for
  a small object, so the place of the code may count more than the allocator.
- **Bytecode**: unchanged.

## R. Constant expressions and constants of a module

2026-11-16, agreed with the user (first steps of a bytecode optimizer). QuickJS computes
nothing at compile time but the minus sign of a number: `5 * 1000`, `1 << 4` and
`"ab" + "cd"` are computed each time they run (a string concatenation allocates). A new
file, `compiler/fold.c`, does two things; the bytecode is the same in debug and in
production, as before.

1. **Constant folding** (`fold_constants()`, between `resolve_variables()` and
   `resolve_labels()`): an operator whose operands are literal numbers or strings is
   replaced, with them, by its result. Operators: `+ - * / % **`, shifts and bit
   operations, unary `- + ~`; `+` joins strings, and a string with a number (the engine
   converts the number). The pass keeps aside the last constants pushed and writes them
   unchanged before any other instruction, a label included, so a value that depends on
   a test is never folded. Values are computed as the interpreter does (doubles, 32 bit
   integers for shifts, `-0` kept). Left to run time: bigints, booleans, `null`,
   `undefined`, comparisons, strings holding half of a surrogate pair. The positions of
   the debugger met among the constants keep their place.
2. **Constants of a module** (`js_find_module_constants()`, before the child functions
   of the module function are made; `resolve_scope_var()` and `add_global_variables()`
   use its result): a `const` declared at the top level of a module with such a value
   (literals, or an expression of literals and of other such constants) is replaced by
   its value wherever the module reads it. When the module does not export it, it has no
   variable at all: no closure variable in the module function nor in the functions that
   read it, no `JSVarRef`, no name, no initialization code. An exported one keeps its
   variable for the modules that import it. A candidate is given up when any function of
   the module assigns to, deletes or takes a reference of that name (even a local
   variable of the same name: names are not resolved yet), and a module that calls
   `eval()` directly keeps all its variables.
- **Agreed changes of behavior**: reading such a constant before its declaration ran
  gives its value (QuickJS throws a ReferenceError); a breakpoint on its line slides to
  the next statement (the line has no code).
- **Debugger**: `JSDebugFunctionInfo.constants` (`quickjs-debug.h`, name and value, with
  the module function); the sidecar has `constants: [{ name, value }]`; the proxy shows
  them in the closure scope of every function of the module, passes them to an
  evaluation, refuses to change them (`host/js/debuginfo.js`, `debugsession.js`).
- **Bytecode**: same opcodes, `BC_VERSION` not bumped.
- **Measures** on the program of a board (17 modules, 216 functions): 86 of its 119
  module variables are such constants; closure variable entries 636 -> 445, names 593 ->
  509, checked reads of module variables 138 -> 15, bytecode 18,873 -> 18,319 bytes,
  package 44,183 -> 41,683 bytes; on the board (ESP32-S3 without PSRAM) its JS heap
  206.0 -> 195.6 KB.
- Tests: `tests/fold.test.js` (82 checks: each expression against the same one computed
  at run time), `tests/modconst.test.js` (21), `tests/debuginfo.test.js` and
  `tests/proxy.test.js` (the constants in the debugger).
- Not done: constants exported from a module replaced in the modules that import them
  (the build would pass them); booleans; dead entries left in the constant pool by a
  folded float.

## S. Package executed in place: names of the package, fixed atom indexes

2026-11-16, agreed with the user. A module of a package carried the table of its own
names; reading it made each name in the heap, then wrote the index of each name in the
bytecode, which therefore had to be copied in the heap. On the ESP32-S3 without PSRAM the
program of a board kept about 19 KB of names and 18 KB of bytecode in RAM, both already
in its package in flash.

- **Fixed atom indexes**: `0 .. QJSX_ROM_ATOM_COUNT - 1` the names of the engine
  (change P), then the names of the package, then the names made at run time.
- **Names of a package** (`core/package-atoms.c`, new):
  - `JS_WritePackageAtoms(ctx, &size)` (engine with compiler) writes the image of the
    names a runtime made after the engine's: a header (mark "QJSN", fingerprint of the
    engine's names, first index, count, hash size, size), the offset of each name, a hash
    table (open addressing, an atom index per slot), then each name as QuickJS allocates
    an atom string, with an immortal reference count and its index in `hash_next`. No
    pointer in it: the same image serves a 32 bit device and the PC;
  - `JS_SetPackageAtoms(rt, image, size)` gives them to a runtime that made no name yet
    (just after `JS_NewRuntime2`, before any context): the atom table points at the
    image, `rt->const_atom_end` moves past them, the lookups of `__JS_NewAtom` /
    `__JS_FindAtom` search the engine's names, then the package's, then the heap's;
    refused (-1) for another fingerprint or another layout. `JS_HasPackageAtoms(rt)`;
  - `JS_GetAtomsFingerprint()`: `QJSX_ROM_ATOM_FINGERPRINT`, written in `rom-atoms.h` by
    `tools/rom-atoms/generate.sh` (a hash of the names and of their order).
- `__JS_AtomIsConst()` takes the runtime (`const_atom_end`); `js_atom_is_rom()` is now
  "its reference count is immortal" (engine's and package's names alike).
- **Bytecode with fixed indexes**: `JS_WRITE_OBJ_FIXED_ATOMS` writes every atom as its
  index in the writing runtime, with an empty table of names and its own first byte
  (`BC_VERSION_FIXED_ATOMS` = `BC_VERSION | 0x80`); `JS_READ_OBJ_FIXED_ATOMS` reads such
  a bytecode only, takes the indexes as they are (checked against `const_atom_end`).
  With `JS_READ_OBJ_ROM_DATA`, QuickJS's own path, the bytecode of the functions is used
  where the buffer lies: nothing in it is changed.
- `struct JSString` has the layout of GCC on Windows too (`JS_GCC_LAYOUT`: 12 bytes
  instead of 20), so that the image is the same on the PC.
- **`BC_VERSION` 0x45 -> 0x46**: a firmware of before refuses the new packages.
- The link (`sys/compiler` `link()`, `host/modules/compiler_module.cpp`): a runtime of
  its own reads the bytecode of every module of the package (their names get the indexes
  that follow the engine's, in order), writes the image of the names, then each module
  with `JS_WRITE_OBJ_FIXED_ATOMS`. `qjsx build` calls it (see architecture.md 4.1).
- Not changed: the bytecode the debugger sends (LOAD_FUNCTION) and a package without
  `names.bin` are read as before, with their own table of names.
- **Measures on the board** (ESP32-S3 without PSRAM, its program of 17 modules,
  2026-11-16): JS heap 195.6 -> 156.3 KB; the package sent to the board 41.7 -> 55.9 KB
  (the names are stored ready to use, with their headers: flash, not RAM); an empty
  program stays at 62.9 KB. The firmware of before refused nothing by mistake: the
  program installed with bytecode 0x45 was not started by the new firmware.
- Not done: the tables of variables of the functions (`vardefs`, `closure_var`) are
  still copied in the heap; the agent does not compare the fingerprint at the upload
  (the loader refuses the package when it starts).

## T. GC threshold capped by the memory limit

2026-11-16, agreed with the user (option A of five: limit given to the engine, threshold
capped; rejected: the runtime lowering the threshold between turns of the loop or running
the GC between callbacks, which miss a long synchronous computation; a GC inside a
failing allocation, unsafe in the middle of building an object). QuickJS runs its cycle
collector only from `js_trigger_gc` (object creation), when the heap passes
`malloc_gc_threshold`: 256 KB at first, then 1.5 x the heap after each GC. The JS region
of an ESP32-S3 without PSRAM holds 248 KB: the heap never reached the threshold, and no
cycle (an object and its callback, a closure that refers to itself) was collected before
the end of the program; a program making such cycles ran out of memory (found with
`stdlib/tasks`, whose first version made one per wait).

- `js_next_gc_threshold(rt, size)` (`core/runtime.c`, new): 1.5 x `size` as QuickJS, but
  when a memory limit is set (`malloc_limit` not -1) never past halfway from `size` to the
  limit. Used by `js_trigger_gc` after each GC, and by `JS_SetMemoryLimit`, which lowers
  the threshold to it (unless the automatic GC is disabled, -1). Without a limit, QuickJS
  behaves as before. Near the limit the GCs come more often (halfway of what is left).
- The runtime gives the limit: `board::jsHeapLimit()` (new in `board.h`; the size of the
  JS region on an ESP32 without PSRAM and on the Teensy 4.0, of `QJSX_JS_HEAP_MB` on the
  PC, 0 elsewhere: PSRAM, malloc, the RP2350 for now), `JS_SetMemoryLimit` in `createRuntime()`
  (`common/runtime/program.cpp`) right after the runtime is made.
- Bytecode unchanged: no BC_VERSION bump.
- `tests/gclimit.test.js` (`QJSX_JS_HEAP_MB=1`: 450 pieces of live data, then 60,000
  cycles): with the original threshold 400 to 600 pieces run out of memory, with the cap
  they all pass. Bellard's tests 7/7.
- On the board (ESP32-S3 without PSRAM, 248 KB region, 2026-11-16), a probe making 20,000
  cycles (an object, an array of 4 and a closure that refer to each other): the firmware
  of before runs out of memory (it cannot even format the message); with T, 2341 ms,
  against 475 ms for 20,000 objects without cycle. The difference also holds the cost of
  making a closure in a loop (about 1.1 s for 20,000 on that board): the GC itself is
  roughly 0.7 s for some 50 passes, about 13 ms a pass with a small live heap. The
  client's program (193 KB with its sub boards powered) runs as before.

## U. Interrupt on request and the running stack, for a sampling profiler

2026-11-16, agreed with the user (the profiler of the agent, `qjsx profile`, compiled
with `QJSX_PROFILER`: on the PC by default, `idf.py -DQJSX_PROFILER=ON` for a firmware).

- `JS_RequestInterrupt(ctx)` (`quickjs.h`, `core/errors.c`): stores 0 in the interrupt
  counter of the context, so that the interpreter calls the interrupt handler at its
  next check (a jump or a call) instead of after thousands of them. Any thread may call
  it (the sampling thread of the profiler); a request that races with the interpreter's
  decrement may be lost, the next one comes.
- `JS_DebugSampleStack(ctx, frames, max)` (`quickjs-debug.h`, `core/debug.c`): the
  bytecode frames running now, innermost first (module name atom, function number),
  native frames skipped. Unlike `JS_DebugGetFrame`, which reads the frames of a pause of
  the debugger only, it walks `rt->current_stack_frame`: the interrupt handler calls it.
- Bytecode unchanged; nothing runs unless the profiler asks. Bellard's tests 7/7.

## V. The await of async functions without allocation (V1) and without promise (V2)

2026-11-17, agreed with the user (an async function has one await at a time: what
resumes it is kept with it, "un stock d'await" made once per call; and `await f()` of an
async function needs no promise). The programs of a board are tasks that wait: before V,
each `await` of a native promise made two function objects ("resume" on fulfillment and
on rejection), two reaction records and a job, freed at once after; each call of an async
function made its result promise, its two resolving functions and their shared data,
even when the caller only awaited it.

- **V1, the await kept in the state** (`JSAsyncFunctionState`, `quickjs-internal.h`): two
  embedded reactions (`JSAwaitReaction`: a list link and the state; the promise's
  reaction lists hold them next to the `JSPromiseReactionData`, whose new second field
  `awaiter` is NULL), one embedded job (`await_job`, `argc` = `JS_JOB_ARGC_AWAIT`), the
  value or the exception it delivers (`await_value`, `await_throw`).
  - `await` of a native promise whose `constructor` is `%Promise%` (read once, as
    `PromiseResolve` does): `js_promise_await()` (`builtins/promise.c`) links the embedded
    reactions (each holds a reference on the state) or, the promise settled, queues the
    embedded job at once; at settlement `fulfill_or_reject_promise` queues it where the
    reaction job would be. `await` of a value that is not an object: the job queued at
    once (the promise the standard makes is already fulfilled: one turn). Any other
    object (a thenable, a subclass of `Promise`): QuickJS's path, unchanged
    (`js_promise_resolve_new()`: the end of `js_promise_resolve()`, so that the
    `constructor` of a native promise of another constructor is not read twice).
  - `JS_ExecutePendingJob` runs an embedded job with `js_async_function_await_job()`
    (`core/generators.c`, what `js_async_function_resolve_call` did) and does not free it;
    `JS_FreeRuntime` frees what one still holds. The promise finalizer and the cycle
    collector unlink the embedded reactions, never the state (whose memory stays valid
    while a reaction references it); the promise's mark function marks the state; the
    state marks `await_value`.
- **V2, `await f()` without promise.** The interpreter passes `JS_CALL_FLAG_AWAITED`
  (`quickjs-internal.h`) to `OP_call` / `OP_call_method` when the next instruction is
  `OP_await` in an async function (the compiler always emits them side by side; bound
  functions, `apply`, `new`, `call` from C do not pass it on). `js_async_function_call`,
  when the calling frame is an async function's, makes no promise: the callee keeps its
  caller as `awaiter` (one reference), the caller's `await_inline` says that its next
  await is already arranged, and the call returns a placeholder. At its end the callee
  (`js_async_function_settle()`) queues the caller's embedded job with its result or its
  exception: one turn of the queue, as the promise gave. A result that is an object may
  be a thenable, which the standard resolves through extra jobs: the promise is made then,
  the caller awaits it (V1), it is resolved with the result as before. The callee marks
  and frees its `awaiter`.
- What the program sees does not change: the order of the resumptions is upstream's
  (`tests/fixtures/awaitorder.js`, its output compared in `tests/run-tests.sh` with
  `awaitorder.expected`, written by `qjs-original`: values, settled / pending / rejected
  promises, a subclass, a `constructor` getter read once, thenables, `await f()`
  returning at once, a promise, a thenable, throwing at once or later, three levels, a
  method). A synchronous throw of an awaited callee no longer goes through the rejection
  tracker (before: reported "unhandled", then "handled" at the await: nothing printed
  either way). Async generators keep QuickJS's path. Bytecode unchanged: no BC_VERSION
  bump.
- Measured 2026-11-17 with `temp/awaitbench` (JS heap of tasks waiting at an `await` in a
  chain of async calls; time of an `await f()`; one turn handed between two tasks), on the
  PC (YD twin, Release) and on the Teensy 4.0; a pending promise alone is unchanged (373 B
  on the PC, 244 B on the Teensy):

  | | PC before | PC after | Teensy before | Teensy after |
  |---|---:|---:|---:|---:|
  | RAM per task waiting, depth 1 | 1040 B | 783 B | 731 B | 557 B |
  | RAM per task waiting, depth 3 | 3326 B | 1571 B | 2175 B | 989 B |
  | `await f()`, f returns at once | 0.60 us | 0.10 us | 48.75 us | 16.75 us |
  | `await g()`, g awaits f | 1.25 us | 0.15 us | 87.15 us | 25.20 us |
  | hand-over between two tasks | 1.35 us | 0.50 us | 86.85 us | 37.10 us |

  On the YD-ESP32-S3 (JS heap in PSRAM), firmware of the board before (of an unknown older
  date: the gain adds V and what changed since) and after: `await f()` 233.2 -> 7.75 us,
  `await g()` 299.6 -> 14.70 us, hand-over 502.2 -> 224.6 us, RAM per task waiting 667 ->
  501 bytes (depth 1), 2103 -> 1028 bytes (depth 3). The times of the Teensy move with the
  layout of the code in its instruction cache (`doc/known-bugs.md`, "Speed of the Teensy
  4.0"): 6.20 us for `await f()` after the coding rules pass, 3.50 us with the path in the
  ITCM.

## W. Outline of a module, to leave out what nothing uses

2026-11-17, agreed with the user. On a device each function costs RAM when its module
is loaded, called or not (measured on the Teensy 4.0: about 275 bytes, 150 of them its
bytecode object, the rest its function object; a class method about 207 bytes), and
`qjsx build` copied every library module whole. The compiler now describes each module
it parses; the tool (`host/js/prune.js`) leaves out of the package the top-level
declarations that nothing uses and checks the imported names. Compiler only: nothing
changes on the devices, the bytecode format neither.

- **API** (`quickjs-debug.h`): `JS_SetModuleOutlineCollector(rt, collector, opaque)`;
  the collector gets a `JSModuleOutline` once the module is parsed (nothing on a syntax
  error): the top-level statements (`JSOutlineStatement`: byte range, `removable`, the
  names declared, the names used), the imports (`JSOutlineImport`: module, imported name,
  local name, position of the imported name), the exports (`JSOutlineExport`: exported
  name, local name, module of a re-export, range of a name of an export clause with its
  comma) and `has_eval`. Host: option `outline: true` of `compile()` (sys/compiler).
- **Collection** (`compiler/outline.c`, new; `JSParseState.outline`):
  - `js_parse_program` calls `js_outline_statement_begin` / `_end` around each top-level
    statement of a module; `__JS_EvalInternal` (`compiler/eval.c`) starts the outline,
    reports it after a parse without error, frees it whatever the result;
  - names used: every variable access the parser emits (`emit_atom` after an
    `OP_scope_*` opcode, `emit_u32` of an identifier in `parser-expr.c`), in nested
    functions too, each once per statement; the name is taken as written (an inner
    variable of the same name counts: more is kept, never less). The initialization of a
    declared name (`OP_scope_put_var_init`) is not a use;
  - names declared: the module's top-level variables (`fd->global_vars`) added by the
    statement;
  - removable: it declared names, imported nothing, and the bytecode it emitted in the
    module function only creates values (`outline_only_creates`: literals, functions,
    objects, arrays, classes, reads of variables and properties, initializations, and
    assignments to its own names only). A call, `new`, an assignment to another name, a
    computed key that calls, a static field or block (a call of their initializer) make
    it kept. Accepted: a getter that a left-out declaration would have run (key of a
    computed method, property read) is not run;
  - imports: `add_import` (`parser-module.c`) gets the position of the imported name;
    exports: the range of each name of an export clause, with its comma;
  - a direct `eval` (`js_compile_warning`, kind EVAL) sets `has_eval`.
- **In the tool**: see `doc/architecture.md` 4.5 and `host/js/prune.js`: the names used are
  followed from every statement that is kept, through imports, re-exports and
  `export * from`; `import * as ns` uses every export of the module; a module with
  `eval` keeps everything. What is unused is replaced by spaces (line ends kept: same
  positions), the module compiled again; the package keeps the original source.
- **Measured** on the Teensy 4.0 (`temp/deadfn`, JS heap after start): 99 unused
  functions and a class of 20 methods 98,696 -> 65,344 bytes (the same bytecode as the
  source without them); `import { Mutex } from "stdlib/tasks"` 82,320 -> 67,016 bytes.
- Tests: `tests/prune.test.js` (outline, what the build leaves out, the check of the
  imports), `tests/fixtures/prune` run from its sources and from its package (with and
  without compiler) in `tests/run-tests.sh`.

## X. A stock of states of async functions

2026-11-17, agreed with the user (points 1 to 7, 8 entries, 2 KB). Each call of an async
function allocates its state (the `JSAsyncFunctionState` and its frame) and frees it when
the function ends. On a board with a small heap, the state is above the 128 bytes of the
arenas (Q): the board's heap, a `memset`, then the free. The native profiler of the Teensy
put this at about 20 % of an `await f()` with the code in the ITCM, about 45 % from the
flash.

- Options (`quickjs-internal.h`): `JS_ASYNC_STATE_CACHE` (entries; 0: QuickJS's original,
  the default) and `JS_ASYNC_STATE_CACHE_BYTES` (2048). Set by `QJSX_SMALL_HEAP_DEFINES`
  (`ports/cmake/qjsx.cmake`): 8 entries, 2 KB: the boards without PSRAM (Teensy 4.0, an
  ESP32 without PSRAM). A board with PSRAM keeps QuickJS's arenas up to 512 bytes, which
  hold the state already.
- `core/generators.c`: `async_state_alloc()` takes the newest kept state of the exact size
  (`JSAsyncFunctionState.alloc_size`: the size only depends on the function and its
  argument count, so a loop of awaits finds its blocks again), else allocates;
  `async_state_release()` (in `__async_func_free`) keeps the block when it fits in the
  table, the oldest going back to the heap first, else frees it;
  `js_async_state_cache_flush()` gives them all back. The table is in the runtime
  (`async_state_cache`, oldest first).
- Given back: when a large allocation fails (`js_malloc_large`, then one more try, as the
  arenas kept by Q; the runtime is found from its first member, `malloc_ctx`) and in
  `JS_FreeRuntime` after its last GC. The kept blocks count in the JS heap (they are taken).
- What the program sees does not change. Generators use the same states and the same stock.
- Tests: the Debug suite with the settings of a small heap (`CFLAGS` with the options, JS
  heap in a `RegionHeap`: `QJSX_JS_HEAP_MB=64`): 115 OK; Bellard's tests with the same
  options: OK. Not built on the PC by default.
- Measured on the Teensy 4.0 (same layout, the ITCM experiment of `doc/known-bugs.md` in
  place): `await f()` 3.50 -> 2.80 us, `await g()` 6.65 -> 5.10 us; `RegionHeap` gone from
  the profile. On the client board (ESP32-S3 without PSRAM, JS heap in a 248 KB region),
  the same firmware without then with X (`temp/awaitbench-small`: 30 tasks instead of 100,
  which do not fit in the region): `await f()` 10.25 -> 7.20 us, `await g()` 19.70 ->
  13.80 us, hand-over between two tasks 192 -> 133 us (-30 % each); the RAM per waiting
  task is unchanged (the kept states are at most 2 KB in all).

## Y. Import attributes of the requests of a module, in the outline

2026-11-17. A program imports a file as its bytes, `import font from "./font.bin" with
{ type: "bytes" }` (`doc/architecture.md` 4.1, `host/js/bytes.js`). The parser already
reads the `with` clause into `JSReqModuleEntry.attributes` and the module loader gets
it (`JS_SetModuleLoaderFunc2`), but the tool could not see it before the program runs,
nor where the import is written. Compiler only: nothing changes on the devices.

- **API** (`quickjs-debug.h`): `JSModuleOutline` (W) gains `requests` /
  `request_count`, one `JSOutlineRequest` per module request, in the order of
  `JS_GetModuleRequestName` (J): the module as written, its `attributes`
  (`JS_UNDEFINED` without `with`, else the object of strings, valid during the call)
  and the byte offset of the string that names the module. Host: `outline.requests` of
  `compile()` (sys/compiler), `attributes` null without `with`.
- **Collection** (`compiler/outline.c`, `compiler/parser-module.c`):
  `js_parse_from_clause` and the `import "x"` form of `js_parse_import` note where the
  string naming the module is (`js_outline_request_position`); `js_outline_report`
  gives the requests. `outline_reserve()` now grows the three arrays of positions and
  ranges of the outline (imports, exports, requests).
- **Bytecode unchanged**: the outline is not written; the attributes were already
  serialized with the module (`bytecode/writer.c`, upstream). `BC_VERSION` not bumped.
  Checked by hashing, with the executable before and after the change, the bytecode
  of three modules (one with `with` clauses) as compiled and as linked: identical
  (`temp/bytes/bytecode-probe.js`).
- **In the tool**: `qjsx:compile` checks the attributes of every import (only `type:
  "bytes"`, else an error at the position of the module string) and marks the imports
  of bytes; `qjsx build` copies their files into the package (`bytes/<module name>`).
- Tests: `tests/bytes.test.js`, `tests/fixtures/bytes` run from its sources and from its
  package (with and without compiler) in `tests/run-tests.sh`.

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
| + temporal dead zone checks removed (N), 2026-11-12 | 4,642.8 | 4,449.0 | −4.2% |
| + names in read-only memory (P), 2026-11-13 | 4,641.7 | 4,571.0 | −1.5% |
| + constant folding, constants of a module (R), 2026-11-16 | 4,638.5 | 4,532.7 | −2.3% |
| + the await without allocation, `await f()` without promise (V), 2026-11-17 | 4,636.6 | 4,451.6 | −4.0% |

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
| `compiler/tdz.c` (new), `compiler/compiler.h`, `compiler/finalize.c`, `compiler/resolve-labels.c`, `compiler/debug-info.c` (also) | temporal dead zone checks removed where useless; `set_x` fusion not across a stop position (N) |
| `builtins/math.c`, `quickjs.h` (also) | `JS_SetRandomSeed` (O) |
| `quickjs.h`, `quickjs-internal.h`, `core/atoms.c`, `core/runtime.c`, `core/gc.c` (also), `core/rom-atoms.h` and `core/rom-atoms.c` (new, generated) | names in read-only memory, immortal reference count (P) |
| `core/package-atoms.c` (new), `quickjs.h`, `quickjs-internal.h`, `core/atoms.c`, `bytecode/reader.c`, `bytecode/writer.c` (also) | names of a package, bytecode with fixed atom indexes read in place, `JSString` layout, `BC_VERSION` 0x46 (S) |
| `compiler/fold.c` (new), `compiler/compiler.h`, `compiler/finalize.c`, `compiler/funcdef.c`, `compiler/resolve-vars.c`, `compiler/resolve-labels.c`, `compiler/debug-info.c`, `quickjs-debug.h` (also) | constant folding, constants of a module replaced by their value (R) |
| `quickjs-internal.h`, `core/runtime.c` (also) | arenas of the allocator tuned for a small heap, arenas kept (Q); `quickjs.h`, `quickjs-internal.h`, `core/runtime.c`: measure of the block sizes (`QJSX_ALLOC_HISTOGRAM`) and map of the arenas (`QJSX_MEMORY_MAP`) |
| `core/runtime.c` (also) | `js_next_gc_threshold`, GC threshold capped by the memory limit (T) |
| `quickjs.h`, `core/errors.c`, `quickjs-debug.h`, `core/debug.c` (also) | `JS_RequestInterrupt`, `JS_DebugSampleStack` (U) |
| `quickjs-internal.h`, `builtins/promise.c`, `builtins/builtins.h`, `core/generators.c`, `core/core.h`, `core/gc.c`, `core/runtime.c`, `core/interpreter.c` (also) | the await kept in the state of an async function, `await f()` without promise (V) |
| `quickjs-internal.h`, `core/generators.c`, `core/core.h`, `core/runtime.c` (also) | stock of states of async functions (X) |
| `compiler/outline.c` (new), `quickjs-debug.h`, `quickjs-internal.h`, `compiler/compiler.h`, `compiler/eval.c`, `compiler/emitter.c`, `compiler/parser-func.c`, `compiler/parser-expr.c`, `compiler/parser-module.c`, `compiler/debug-info.c` (also) | outline of a module: statements, names declared and used, imports, exports (W) |
| `quickjs-debug.h`, `compiler/outline.c`, `compiler/compiler.h`, `compiler/parser-module.c` (also) | requests of a module in the outline: import attributes and position (Y) |
