# Map of quickjs.c

> The split made from this map is described at the end, in the section
> [Split version](#split-version-engineqjsx).

Mapped version: QuickJS 2026-06-04 by Fabrice Bellard, `engine/qjs-bellard/quickjs.c`.
The file is 61,197 lines long: 1,562 functions, 142 types, 97 data tables.
Line numbers refer to this version.

The complete list of items (module, kind, name, lines) is in [quickjs-items.csv](quickjs-items.csv).

## Method and limits

- Top-level items (functions, types, tables) are extracted by following the braces,
  after removing comments and strings.
- An item "references" another one if its name appears in its body: call, address taken,
  use of a type. The counters in the table are numbers of references, not of calls.
- References made through a macro are not seen. Example: `short_opcode_info(op)`
  hides an access to `opcode_info`.
- The boundaries between modules were chosen by hand, by reading the ordered list of functions.
  A module may occupy several ranges: the file is not always organized by topic.
- `prelude` groups lines 1 to 1415: options, central structures (`JSRuntime`, `JSContext`,
  `JSObject`, `JSFunctionBytecode`, `JSStackFrame`…) and prototypes. It is used by everything
  and does not appear in the dependency columns.

## Modules

| Module | Lines of quickjs.c | Lines | Functions | Depends mostly on | Used mostly by |
|---|---|---:|---:|---|---|
| prelude | 1-1415 | 1415 | 0 | | builtins/typedarray (105), properties (98), gc (74), modules (64) |
| malloc | 1416-1956 | 541 | 32 | errors (4), gc (1) | gc (27), compiler/passes (24), shapes (21), runtime (20) |
| runtime | 1957-2861 | 905 | 49 | malloc (20), gc (19), call-support (7), builtins/iterator (6) | atoms (13), properties (11), builtins/string (11), modules (9) |
| atoms | 2862-3815 | 954 | 49 | malloc (16), runtime (13), strings (7), conversions (2) | properties (61), compiler/parser (40), modules (26), compiler/passes (20) |
| classes | 3816-3922 | 107 | 5 | atoms (4), malloc (1) | runtime (1), builtins/proxy (1), builtins/promise (1), builtins/weakref (1) |
| strings | 3923-5118 | 1196 | 49 | malloc (11), atoms (8), runtime (7), conversions (5) | builtins/string (81), builtins/regexp (36), builtins/json (20), builtins/uri (20) |
| shapes | 5119-5612 | 494 | 24 | malloc (21), gc (7), atoms (4) | properties (18), gc (8), objects (7), builtins/regexp (5) |
| objects | 5613-6096 | 484 | 26 | properties (7), shapes (7), malloc (6), atoms (5) | call-support (14), builtins/promise (13), builtins/regexp (9), properties (8) |
| gc | 6097-7348 | 1252 | 41 | malloc (27), shapes (8), objects (3), runtime (3) | runtime (19), properties (19), call-support (14), shapes (7) |
| errors | 7349-7885 | 537 | 36 | atoms (6), properties (4), objects (4), gc (2) | builtins/typedarray (40), properties (38), builtins/regexp (21), bytecode-io (20) |
| properties | 7886-11062 | 3177 | 84 | atoms (61), errors (38), gc (19), shapes (18) | builtins/array (57), builtins/object (55), builtins/proxy (52), call-support (36) |
| conversions | 11063-11292, 12763-13691 | 1159 | 45 | errors (7), strings (5), bigint (4), malloc (4) | builtins/string (46), builtins/typedarray (30), builtins/number (26), builtins/regexp (23) |
| bigint | 11293-12762 | 1470 | 52 | malloc (13), errors (4), conversions (1), strings (1) | operators (34), builtins/bigint (6), conversions (4), bytecode-io (3) |
| print | 13692-14555 | 864 | 31 | runtime (4), malloc (3), strings (3), atoms (3) | runtime (4), bytecode-io (4), gc (3), compiler/passes (3) |
| operators | 14556-16109 | 1554 | 37 | bigint (34), conversions (22), properties (8), strings (6) | interpreter (17), builtins/array (8), builtins/typedarray (8), builtins/atomics (6) |
| call-support | 16110-17771 | 1662 | 45 | properties (36), malloc (16), errors (15), objects (14) | interpreter (16), builtins/array (14), builtins/iterator (14), builtins/mapset (14) |
| interpreter | 17772-20775 | 3004 | 10 | properties (26), operators (17), call-support (16), errors (16) | builtins/promise (17), builtins/proxy (14), builtins/array (13), builtins/mapset (12) |
| generators | 20776-21667 | 892 | 31 | malloc (13), errors (11), properties (8), builtins/promise (8) | builtins/promise (8), runtime (3), gc (2), modules (2) |
| compiler/tokenizer | 21668-23155, 23530-23691 | 1650 | 24 | strings (18), atoms (4), malloc (3), conversions (2) | compiler/parser (148), compiler/passes (33), compiler/emitter (24), json-tokenizer (6) |
| json-tokenizer | 23156-23529 | 374 | 4 | compiler/tokenizer (6), strings (5), runtime (3), atoms (1) | builtins/json (3) |
| compiler/emitter | 23692-24357 | 666 | 37 | compiler/tokenizer (24), atoms (6), malloc (6), errors (2) | compiler/parser (189), compiler/passes (17), eval-api (1) |
| compiler/parser | 24358-29546, 31477-31944, 36210-37030 | 6478 | 70 | compiler/emitter (189), compiler/tokenizer (148), atoms (40), properties (10) | eval-api (2), compiler/passes (1), builtins/json (1) |
| modules | 29547-31476 | 1930 | 58 | atoms (26), malloc (19), errors (16), runtime (9) | compiler/parser (8), eval-api (7), gc (2), interpreter (2) |
| compiler/passes | 31945-36209 | 4265 | 47 | compiler/tokenizer (33), malloc (24), atoms (20), compiler/emitter (17) | compiler/parser (5), eval-api (4), gc (1) |
| eval-api | 37031-37261 | 231 | 8 | modules (7), compiler/tokenizer (4), compiler/passes (4), errors (2) | builtins/global (2), interpreter (1), modules (1), builtins/function (1) |
| bytecode-io | 37262-39400 | 2139 | 63 | errors (20), malloc (17), atoms (14), builtins/typedarray (6) | builtins/typedarray (2) |
| cfunc-lists | 39401-39762 | 362 | 12 | properties (6), objects (6), atoms (5), modules (2) | builtins/promise (9), builtins/array (7), intrinsics (5), builtins/mapset (4) |
| builtins/global | 39763-39789, 55947-55954 | 35 | 4 | eval-api (2), conversions (2) | builtins/uri (3), builtins/number (2), runtime (1) |
| builtins/object | 39790-40930 | 1141 | 36 | properties (55), errors (18), atoms (10), objects (8) | builtins/array (25), builtins/reflect (6), intrinsics (5), builtins/typedarray (5) |
| builtins/function | 40931-41323 | 393 | 12 | strings (10), properties (7), call-support (6), interpreter (4) | builtins/array (24), intrinsics (4), interpreter (3), builtins/regexp (3) |
| builtins/error | 41324-41481 | 158 | 4 | properties (4), objects (2), conversions (2), errors (2) | intrinsics (4), builtins/promise (2) |
| builtins/array | 41482-43468 | 1987 | 36 | properties (57), builtins/object (25), builtins/function (24), conversions (19) | builtins/iterator (28), builtins/typedarray (6), builtins/json (4), runtime (2) |
| builtins/iterator | 43469-44587 | 1119 | 21 | builtins/array (28), errors (16), call-support (14), properties (13) | intrinsics (9), runtime (6), builtins/promise (1) |
| builtins/number | 44588-44950 | 363 | 20 | conversions (26), errors (6), strings (3), objects (2) | intrinsics (5), builtins/uri (2), builtins/bigint (1) |
| builtins/string | 44951-46697 | 1747 | 48 | strings (81), conversions (46), errors (12), runtime (11) | builtins/regexp (11), intrinsics (4), runtime (2), compiler/parser (2) |
| builtins/math | 46698-47226 | 529 | 18 | call-support (3), conversions (2), errors (1), operators (1) | intrinsics (2) |
| builtins/date | 47227-47326, 54788-55946 | 1259 | 45 | conversions (13), errors (6), objects (4), interpreter (3) | runtime (1), print (1) |
| builtins/regexp | 47327-49091 | 1765 | 38 | strings (36), conversions (23), errors (21), properties (18) | runtime (4), builtins/string (2), interpreter (1) |
| builtins/json | 49092-50184 | 1093 | 24 | strings (20), properties (18), atoms (12), conversions (9) | runtime (1) |
| builtins/reflect | 50185-50352 | 168 | 8 | properties (10), errors (6), builtins/object (6), atoms (4) | intrinsics (1) |
| builtins/proxy | 50353-51340 | 988 | 25 | properties (52), errors (18), interpreter (14), conversions (7) | runtime (1), operators (1), interpreter (1) |
| builtins/symbol | 51341-51464 | 124 | 7 | atoms (4), errors (3), conversions (2), builtins/string (1) | intrinsics (3) |
| builtins/mapset | 51465-53130 | 1666 | 44 | properties (33), call-support (14), interpreter (12), malloc (11) | builtins/promise (17), builtins/weakref (14), runtime (5), intrinsics (2) |
| builtins/promise | 53131-54492 | 1362 | 43 | properties (24), errors (19), builtins/mapset (17), interpreter (17) | generators (8), modules (7), runtime (1), call-support (1) |
| builtins/uri | 54493-54787 | 295 | 11 | strings (20), runtime (6), conversions (4), builtins/global (3) | intrinsics (1) |
| builtins/bigint | 55955-56151 | 197 | 7 | bigint (6), errors (4), operators (3), conversions (2) | intrinsics (1) |
| intrinsics | 56152-56511 | 360 | 2 | builtins/iterator (9), cfunc-lists (5), objects (5), builtins/object (5) | builtins/typedarray (17), builtins/atomics (5), runtime (2), print (1) |
| builtins/typedarray | 56512-60298 | 3787 | 116 | errors (40), properties (30), conversions (30), malloc (17) | builtins/atomics (22), bytecode-io (6), builtins/array (5), runtime (3) |
| builtins/atomics | 60299-60912 | 614 | 12 | builtins/typedarray (22), errors (6), conversions (6), operators (6) | builtins/weakref (4), runtime (1) |
| builtins/weakref | 60913-end | 285 | 12 | builtins/mapset (14), properties (9), malloc (7), runtime (6) | gc (2), runtime (1) |

Biggest functions: `JS_CallInternal` (2,823 lines, line 17772), `resolve_labels` (902),
`js_parse_statement_or_decl` (761), `js_parse_postfix_expr` (667), `js_parse_function_decl2` (567),
`js_parse_class` (511), `resolve_scope_var` (480), `js_parse_destructuring_element` (468),
`next_token` (441), `resolve_variables` (358).

## The compiler (removing eval)

The compiler takes about 13,300 lines, that is 22 % of the file: `compiler/tokenizer`,
`compiler/emitter`, `compiler/parser`, `compiler/passes` and the compilation part of `eval-api`.

### It is already behind an indirection

- The context has a pointer `ctx->eval_internal` (line 555).
- Only `JS_AddIntrinsicEval` (line 55947) fills it, with `__JS_EvalInternal` (line 37071),
  which is the only entry into the compiler.
- Everything else goes through `JS_EvalInternal` (line 37187), which throws a `TypeError`
  "eval is not supported" if the pointer is NULL. This is the case of `JS_Eval`, `JS_EvalThis`,
  `JS_EvalObject`, of the global eval (`js_global_eval`), of the `Function` constructor
  (`js_function_constructor`) and of the direct eval in the interpreter (`JS_CallInternal` →
  `JS_EvalObject`).
- `JS_NewContext` calls `JS_AddIntrinsicEval`. A build without the compiler must start from
  `JS_NewContextRaw` and add only the intrinsics wanted.

### Couplings to cut to take the compiler out of the build

1. **JSON.parse uses the parser state.** `JS_ParseJSON3` uses `JSParseState`,
   `js_parse_init`, `free_token` and `js_parse_error`. The JSON tokenizer (`json_parse_*`,
   `json_next_token`, lines 23156-23529) uses `ident_realloc`, `js_parse_error`,
   `js_parse_error_pos` and `free_token`. A small common "tokenizer" base shared by
   JSON and the compiler is needed, or a state specific to JSON.
2. **`free_function_bytecode`** (line 36166) is placed among the compiler passes, but it
   frees bytecode at run time and the GC calls it (`free_gc_object`). It goes to the runtime side.
3. **`opcode_info` and `short_opcode_info`** (lines 22035-22068) are defined in the compiler
   area, but the bytecode reader and writer use them (`bc_byte_swap`,
   `JS_WriteFunctionBytecode`, `JS_ReadFunctionBytecode`). To be shared. The debugger will also
   need them to walk the bytecode.
4. **`js_parse_error`** is called by `add_export_entry2` in the `modules` module (line 29680).
5. **`eval-api` mixes two roles.** `JS_EvalFunction` / `JS_EvalFunctionInternal` run
   already compiled bytecode (the modules use them) and stay on the runtime side.
   `__JS_EvalInternal` and its callers belong to the compiler.
6. **The compiler types** (`JSFunctionDef`, `JSToken`, `JSParseState`, `BlockEnv`,
   `JSVarDef`…, lines 21668-22057) form a header private to the compiler, except what JSON
   keeps in common (point 1).

### Consequence for the debugger

`Runtime.evaluate` and `Debugger.evaluateOnCallFrame` need the compiler. Decision taken
since then: no ESP build contains the compiler, even in debug. Expressions are
compiled on the host and sent to the ESP as bytecode (see `doc/architecture.md`).

## Points of interest for the debugger

- **Interpreter**: `JS_CallInternal`, lines 17772-20599. A single function, this is where
  the hooks go.
- **Frames**: `JSStackFrame` (line 407), chained from `rt->current_stack_frame`.
- **Positions**: `find_line_num` (line 7435) decodes the `pc2line` table, `build_backtrace`
  (line 7538) walks the frames. The table is produced at compile time by
  `compute_pc2line_info` (line 34450).
- **`debugger` keyword**: it is always ignored by the parser (line 29507, "currently no
  debugger").
- **Interruption**: `js_poll_interrupts` (line 7877) and `JS_SetInterruptHandler` (line 2236).

## Split version (engine/qjsx)

`engine/qjsx/` contains the same engine split into 55 source files. `engine/qjs-bellard/`
stays untouched. Each file states at its top the lines of `quickjs.c` it comes from.
Performance, sizes and compile times: [quickjs-split-perf.md](quickjs-split-perf.md).
The split is produced by the generator of [tools/quickjs-split](../tools/quickjs-split/README.md),
which also provides the test build and the performance comparison.

### Files

| Folder | Role | Files (lines) |
|---|---|---|
| `core/` | always compiled | atoms (936), bigint-arith (1470), cfunc (366), closures (761), conversions (1089), errors (552), gc (1296), generators (860), interpreter (3117), intrinsics (363), iteration (890), modules (1934), objects (982), operators (1562), print (872), properties (3177), runtime (1458), strings (1196) |
| `bytecode/` | loading and saving of the bytecode | opcodes (21), reader (1158), writer (956) |
| `builtins/` | standard objects, one file per object | array (1964), arraybuffer (637), atomics (541), base64 (764), bigint (206), dataview (396), date (1255), function (560), global (329), iterator (1095), json (1577), mapset (1643), math (537), number (372), object (1150), promise (1368), proxy (1165), regexp (1766), string (1753), symbol (133), typedarray (2108), weakref (302) |
| `compiler/` | the compiler, absent from a build without eval | emitter (632), eval (134), finalize (582), funcdef (607), parser-class (686), parser-expr (3140), parser-func (831), parser-module (486), parser-stmt (1346), resolve-labels (1828), resolve-vars (1173), tokenizer (1202) |

The other files of the engine (`cutils`, `dtoa`, `libregexp`, `libunicode`, `quickjs.h`…)
are copied as they are to the root of `engine/qjsx/`.

### Headers

- `quickjs-internal.h`, included by every file. In order:
  - the prelude of `quickjs.c` without its prototypes;
  - the macros and types shared between files;
  - the prototype headers;
  - the shared inline functions.
- `core/core.h`, `bytecode/bytecode.h`, `builtins/builtins.h`: prototypes of the functions and
  `extern` declarations of the tables shared between files. Included by `quickjs-internal.h`.
- `compiler/compiler.h`: types, macros and prototypes private to the compiler. Only the
  files of `compiler/` include it. No other file depends on the compiler, except
  `JS_NewContext`, which calls `JS_AddIntrinsicEval` of the public API.

### Rules of the split

- **Functions and tables**: only those used in several files lost
  `static` (688 functions, 48 tables). The others stay `static` in their file.
- **Size of the shared tables**: it is written in their `extern` declaration, so that
  `countof()` works elsewhere. If a table changes size without the header being updated,
  the compilation fails.
- **Macros defined inside a function**: when they are used elsewhere (for example
  `JS_THROW_VAR_RO` in `JS_CallInternal`), an identical copy is placed in a header.
- **Items moved out of their line area**:
  - `free_function_bytecode` and `free_bytecode_atoms` go to `core/gc.c`;
  - `opcode_info` goes to `bytecode/opcodes.c`;
  - `JS_EvalFunction` and the eval dispatch go to `core/interpreter.c`;
  - `JS_ResolveModule` goes to `core/modules.c`;
  - `ident_realloc` goes to `core/strings.c` and `get_line_col` to `core/errors.c`;
  - `js_get_prototype_from_ctor` goes to `core/objects.c`.

### Code changes compared to quickjs.c

1. **JSON.parse has its own state** (`JSONParseState`, `JSONToken`, `JSON_TOK_*`, in
   `builtins/json.c`), instead of reusing that of the JavaScript parser.
2. **Module exports are separated from the parser**:
   - `add_export_entry2` (`core/modules.c`) no longer receives a `JSParseState` and throws an
     ordinary `SyntaxError`;
   - `add_export_entry` (`compiler/parser-module.c`) reports the duplicate with its position
     in the source, then calls it.

Nothing else changed in the code of the functions.
