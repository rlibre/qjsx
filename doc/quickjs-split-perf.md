# Split QuickJS: performance and compilation

Comparison between `engine/qjs-bellard/quickjs.c` (QuickJS 2026-06-04, a single file)
and `engine/qjsx/` (same code split into 55 files), measured on 2026-11-10.

Conditions: Windows, gcc 13.1.0 mingw64, `-O2 -fwrapv`, without LTO, 20 threads.
Both `qjs.exe` are built with the same `qjs.c`, `quickjs-libc.c` and `repl.c`
from Bellard's distribution.

## Regression tests

Bellard's tests pass with both versions: `test_closure`, `test_language`,
`test_builtin` (`--std`), `test_loop`, `test_bigint`, `test_cyclic_import`, `test_worker`.
The error messages and positions of `JSON.parse` and the duplicate export error are
identical between the two versions.

## Compilation and size

| Measure | Original | Split |
|---|---:|---:|
| Full compilation of the engine, 1 thread | 47.4 s | 45.5 s |
| Full compilation of the engine, 20 threads | 47.4 s | 7.4 s |
| Recompilation after a change to `json.c` | 42.7 s | 1.6 s |
| Recompilation after a change to `interpreter.c` | 42.7 s | 2.7 s |
| Recompilation after a change to `parser-expr.c` | 42.7 s | 1.7 s |
| Recompilation after a change to `quickjs-internal.h` | — | 6.7 s |
| Machine code of the engine (text section) | 1,112 kB | 1,000 kB |
| `qjs.exe` without symbols | 1,256 kB | 1,143 kB |

(1 kB = 1,000 bytes)

## Speed (tests/microbench.js)

Time per operation in nanoseconds, average of two passes per version.
From one pass to the next, the median difference is 0.5 %: the big differences below come
from the generated code, not from measurement noise.

Unverified hypothesis: in a single file, gcc inlines the `static` functions
called only once, which makes `JS_CallInternal` bigger. Once split, these functions can
no longer be inlined across files: smaller code, and an interpreter sometimes
faster, sometimes slower.

| Test | Original (ns) | Split (ns) | Difference |
|---|---:|---:|---:|
| empty_loop | 8.98 | 9.02 | +0.4 % |
| empty_down_loop | 17.66 | 13.14 | -25.6 % |
| empty_down_loop2 | 18.73 | 14.72 | -21.4 % |
| empty_do_loop | 12.93 | 10.36 | -19.9 % |
| date_now | 58.40 | 53.09 | -9.1 % |
| date_parse | 440.68 | 439.96 | -0.2 % |
| prop_read | 11.79 | 6.78 | -42.5 % |
| prop_write | 10.00 | 6.86 | -31.5 % |
| prop_update | 9.93 | 11.10 | +11.8 % |
| prop_create | 34.44 | 35.75 | +3.8 % |
| prop_clone | 43.17 | 55.06 | +27.5 % |
| prop_delete | 79.75 | 89.66 | +12.4 % |
| array_read | 10.13 | 5.22 | -48.4 % |
| array_write | 7.74 | 6.05 | -21.8 % |
| array_update | 10.83 | 10.25 | -5.4 % |
| array_prop_create | 12.66 | 11.99 | -5.3 % |
| array_slice | 18.57 | 17.20 | -7.4 % |
| array_length_read | 11.65 | 7.86 | -32.6 % |
| array_length_decr | 46.40 | 39.30 | -15.3 % |
| array_hole_length_decr | 43.14 | 48.48 | +12.4 % |
| array_push | 36.33 | 29.68 | -18.3 % |
| array_pop | 70.37 | 68.72 | -2.3 % |
| typed_array_read | 12.16 | 7.48 | -38.5 % |
| typed_array_write | 10.87 | 10.63 | -2.3 % |
| arguments_read | 114.69 | 97.03 | -15.4 % |
| arguments_strict_read | 91.37 | 77.48 | -15.2 % |
| global_read | 6.68 | 6.62 | -0.9 % |
| global_write | 7.53 | 6.80 | -9.6 % |
| global_write_strict | 7.54 | 6.78 | -10.1 % |
| local_destruct | 29.99 | 24.27 | -19.1 % |
| global_destruct | 38.92 | 32.81 | -15.7 % |
| global_destruct_strict | 39.52 | 32.66 | -17.4 % |
| global_func_call | 21.22 | 19.54 | -7.9 % |
| func_call | 21.11 | 19.45 | -7.9 % |
| func_closure_call | 29.15 | 23.41 | -19.7 % |
| int_arith | 10.97 | 10.75 | -2.1 % |
| float_arith | 14.15 | 14.81 | +4.7 % |
| map_set_string | 167.79 | 166.28 | -0.9 % |
| map_set_int | 84.85 | 80.66 | -4.9 % |
| map_set_bigint | 113.97 | 114.84 | +0.8 % |
| map_delete | 169.00 | 168.03 | -0.6 % |
| weak_map_set | 61.56 | 57.01 | -7.4 % |
| weak_map_delete | 152.20 | 143.06 | -6.0 % |
| array_for | 15.57 | 11.46 | -26.4 % |
| array_for_in | 42.38 | 38.30 | -9.6 % |
| array_for_of | 21.91 | 21.11 | -3.7 % |
| math_min | 26.02 | 23.84 | -8.4 % |
| regexp_ascii | 149.54 | 155.52 | +4.0 % |
| regexp_utf16 | 150.79 | 165.71 | +9.9 % |
| regexp_replace | 599.91 | 535.89 | -10.7 % |
| string_length | 11.96 | 10.29 | -14.0 % |
| string_build1 | 43.09 | 44.09 | +2.3 % |
| string_build1x | 43.75 | 44.12 | +0.8 % |
| string_build2c | 66.63 | 67.34 | +1.1 % |
| string_build2 | 54.22 | 55.40 | +2.2 % |
| string_build3 | 42.04 | 42.14 | +0.2 % |
| string_build4 | 54.41 | 55.71 | +2.4 % |
| string_build_large1 | 68.83 | 69.28 | +0.6 % |
| string_build_large2 | 51.24 | 52.27 | +2.0 % |
| int_to_string | 33.62 | 28.54 | -15.1 % |
| int_toString | 39.72 | 39.55 | -0.4 % |
| float_to_string | 187.65 | 185.39 | -1.2 % |
| float_toString | 193.82 | 196.13 | +1.2 % |
| float_toFixed | 81.00 | 77.09 | -4.8 % |
| float_toPrecision | 87.70 | 86.37 | -1.5 % |
| float_toExponential | 87.77 | 83.71 | -4.6 % |
| string_to_int | 57.28 | 65.25 | +13.9 % |
| string_to_float | 78.71 | 80.53 | +2.3 % |
| bigint32_arith | 31.16 | 26.17 | -16.0 % |
| bigint64_arith | 42.49 | 37.73 | -11.2 % |
| bigint256_arith | 98.36 | 93.50 | -4.9 % |
| sort_bench | 16.48 | 17.10 | +3.7 % |
| **total** | 4697.55 | 4520.19 | -3.8 % |

## Engine modified for the debugger (2026-11-10)

Same method, 3 passes per version, no debugger attached. The changes are described in
[engine-changes.md](engine-changes.md). Columns:

- **Original**: Bellard's `quickjs.c` (measured on 2026-11-10, same session as "Debugger");
- **Split**: the split alone, table above;
- **Debugger**: the split with the debugger changes (final version);
- the two differences compare "Debugger" to the original and to the split alone. Negative = faster.

| Test | Original (ns) | Split (ns) | Debugger (ns) | vs original | vs split |
|---|---:|---:|---:|---:|---:|
| empty_loop | 9.04 | 9.02 | 8.64 | -4.4 % | -4.2 % |
| empty_down_loop | 17.34 | 13.14 | 13.20 | -23.9 % | +0.5 % |
| empty_down_loop2 | 18.73 | 14.72 | 14.70 | -21.5 % | -0.1 % |
| empty_do_loop | 12.96 | 10.36 | 9.93 | -23.4 % | -4.2 % |
| date_now | 58.33 | 53.09 | 52.51 | -10.0 % | -1.1 % |
| date_parse | 431.40 | 439.96 | 453.02 | +5.0 % | +3.0 % |
| prop_read | 11.56 | 6.78 | 6.60 | -42.9 % | -2.7 % |
| prop_write | 9.16 | 6.86 | 6.62 | -27.7 % | -3.5 % |
| prop_update | 9.64 | 11.10 | 11.00 | +14.1 % | -0.9 % |
| prop_create | 34.22 | 35.75 | 35.40 | +3.4 % | -1.0 % |
| prop_clone | 42.97 | 55.06 | 51.40 | +19.6 % | -6.6 % |
| prop_delete | 78.97 | 89.66 | 86.83 | +10.0 % | -3.2 % |
| array_read | 9.97 | 5.22 | 5.48 | -45.0 % | +5.0 % |
| array_write | 7.68 | 6.05 | 6.04 | -21.4 % | -0.2 % |
| array_update | 10.76 | 10.25 | 10.02 | -6.9 % | -2.2 % |
| array_prop_create | 12.54 | 11.99 | 11.61 | -7.4 % | -3.2 % |
| array_slice | 18.35 | 17.20 | 17.23 | -6.1 % | +0.2 % |
| array_length_read | 11.48 | 7.86 | 7.80 | -32.1 % | -0.8 % |
| array_length_decr | 45.83 | 39.30 | 38.43 | -16.1 % | -2.2 % |
| array_hole_length_decr | 42.51 | 48.48 | 47.19 | +11.0 % | -2.7 % |
| array_push | 37.74 | 29.68 | 29.63 | -21.5 % | -0.2 % |
| array_pop | 70.77 | 68.72 | 67.95 | -4.0 % | -1.1 % |
| typed_array_read | 12.21 | 7.48 | 7.20 | -41.0 % | -3.7 % |
| typed_array_write | 10.91 | 10.63 | 10.54 | -3.4 % | -0.8 % |
| arguments_read | 112.51 | 97.03 | 97.89 | -13.0 % | +0.9 % |
| arguments_strict_read | 91.91 | 77.48 | 74.65 | -18.8 % | -3.7 % |
| global_read | 6.71 | 6.62 | 6.55 | -2.4 % | -1.1 % |
| global_write | 7.52 | 6.80 | 6.62 | -12.0 % | -2.6 % |
| global_write_strict | 7.53 | 6.78 | 6.62 | -12.1 % | -2.4 % |
| local_destruct | 29.67 | 24.27 | 23.26 | -21.6 % | -4.2 % |
| global_destruct | 39.04 | 32.81 | 31.91 | -18.3 % | -2.7 % |
| global_destruct_strict | 38.93 | 32.66 | 31.84 | -18.2 % | -2.5 % |
| global_func_call | 21.06 | 19.54 | 18.85 | -10.5 % | -3.5 % |
| func_call | 21.02 | 19.45 | 18.84 | -10.4 % | -3.1 % |
| func_closure_call | 29.07 | 23.41 | 22.37 | -23.0 % | -4.4 % |
| int_arith | 10.94 | 10.75 | 10.49 | -4.1 % | -2.4 % |
| float_arith | 13.88 | 14.81 | 14.37 | +3.5 % | -3.0 % |
| map_set_string | 165.05 | 166.28 | 161.35 | -2.2 % | -3.0 % |
| map_set_int | 84.17 | 80.66 | 79.20 | -5.9 % | -1.8 % |
| map_set_bigint | 113.79 | 114.84 | 114.48 | +0.6 % | -0.3 % |
| map_delete | 167.24 | 168.03 | 167.68 | +0.3 % | -0.2 % |
| weak_map_set | 60.60 | 57.01 | 54.31 | -10.4 % | -4.7 % |
| weak_map_delete | 150.12 | 143.06 | 142.67 | -5.0 % | -0.3 % |
| array_for | 15.53 | 11.46 | 11.03 | -29.0 % | -3.8 % |
| array_for_in | 41.97 | 38.30 | 37.95 | -9.6 % | -0.9 % |
| array_for_of | 21.85 | 21.11 | 21.15 | -3.2 % | +0.2 % |
| math_min | 25.50 | 23.84 | 23.06 | -9.6 % | -3.3 % |
| regexp_ascii | 148.84 | 155.52 | 157.06 | +5.5 % | +1.0 % |
| regexp_utf16 | 152.51 | 165.71 | 168.01 | +10.2 % | +1.4 % |
| regexp_replace | 601.04 | 535.89 | 605.63 | +0.8 % | +13.0 % |
| string_length | 11.83 | 10.29 | 10.06 | -15.0 % | -2.2 % |
| string_build1 | 43.35 | 44.09 | 43.65 | +0.7 % | -1.0 % |
| string_build1x | 43.26 | 44.12 | 43.67 | +0.9 % | -1.0 % |
| string_build2c | 67.93 | 67.34 | 65.82 | -3.1 % | -2.3 % |
| string_build2 | 53.72 | 55.40 | 54.43 | +1.3 % | -1.8 % |
| string_build3 | 42.08 | 42.14 | 41.95 | -0.3 % | -0.5 % |
| string_build4 | 54.59 | 55.71 | 54.89 | +0.5 % | -1.5 % |
| string_build_large1 | 67.81 | 69.28 | 66.58 | -1.8 % | -3.9 % |
| string_build_large2 | 51.03 | 52.27 | 50.28 | -1.5 % | -3.8 % |
| int_to_string | 33.29 | 28.54 | 28.51 | -14.4 % | -0.1 % |
| int_toString | 39.61 | 39.55 | 39.62 | +0.0 % | +0.2 % |
| float_to_string | 187.26 | 185.39 | 182.31 | -2.6 % | -1.7 % |
| float_toString | 192.20 | 196.13 | 194.34 | +1.1 % | -0.9 % |
| float_toFixed | 80.25 | 77.09 | 76.52 | -4.6 % | -0.7 % |
| float_toPrecision | 86.82 | 86.37 | 84.92 | -2.2 % | -1.7 % |
| float_toExponential | 86.98 | 83.71 | 82.87 | -4.7 % | -1.0 % |
| string_to_int | 56.51 | 65.25 | 64.19 | +13.6 % | -1.6 % |
| string_to_float | 77.33 | 80.53 | 72.77 | -5.9 % | -9.6 % |
| bigint32_arith | 30.88 | 26.17 | 26.11 | -15.4 % | -0.2 % |
| bigint64_arith | 42.28 | 37.73 | 37.52 | -11.3 % | -0.6 % |
| bigint256_arith | 97.38 | 93.50 | 92.46 | -5.1 % | -1.1 % |
| sort_bench | 15.96 | 17.10 | 16.02 | +0.4 % | -6.3 % |
| **total** | 4665.36 | 4520.19 | 4546.30 | -2.6 % | +0.6 % |

### Gains and losses to analyze later

Nothing of what follows is explained: these are leads for the analysis of the engine
optimizations (TODO.md).

- **The split speeds up reads**: `array_read` −45 %, `prop_read` −43 %,
  `typed_array_read` −41 %, `array_length_read` −32 %, `array_for` −29 %, `prop_write` −28 %,
  and all the empty loops (−20 to −24 %, except `empty_loop`). These gains already existed with
  the split alone; the debugger changes do not change them.
- **It slows down a few operations**: `prop_clone` +20 %, `prop_update` +14 %,
  `string_to_int` +14 %, `array_hole_length_decr` +11 %, `regexp_utf16` +10 %, `prop_delete`
  +10 %. They too were already there with the split alone.
- **The debugger changes** cost +0.6 % on the total compared to the split alone
  (from −10 % for `string_to_float` to +5 % per test, plus `regexp_replace` +13 %, which is
  noise: see below). The double dispatch table alone
  (without the position markers) measured −4.1 % on the total against the original, the final
  version −2.6 %.
- **Measurement noise**: the tests that spend most of their time in C (regular expressions,
  conversions) vary by up to ±10 % from one pass to the next. `regexp_replace` measured 528 ns
  then 605 ns for the same binary.
- **Hypothesis to check** (already noted above): the inlining of `static` functions changes
  with the split, which changes the size of `JS_CallInternal` and the placement of the code.
  Possible checks:
  - compare the machine code of the opcode cases concerned;
  - measure with LTO;
  - see whether the order or the alignment of the dispatch cases matters (TODO.md).
