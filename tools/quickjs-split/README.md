# quickjs-split

Generates `engine/qjsx/` (QuickJS split into 55 source files) from
`engine/qjs-bellard/quickjs.c`, and checks that the result behaves like the original.
The split itself is described in `doc/quickjs-map.md`, section "Split version".

## Files

| File | Role |
|---|---|
| `split.js` | the generator: cuts quickjs.c into top-level items, applies the patches, decides what is shared between files, writes the files |
| `split-config.js` | line ranges of each output file, items moved elsewhere, source patches (JSON parser state, module exports) |
| `regen.sh` | regenerates `engine/qjsx/` and copies the other engine files (cutils, dtoa, libregexp...) |
| `test/premake5.lua` | builds `qjs` with the original engine and with the split one |
| `test/run.sh` | runs the QuickJS tests on both, and optionally `microbench.js` |
| `test/bench-table.js` | turns the microbench outputs into a Markdown comparison table |

`split-config.js` refers to line numbers of the quickjs.c of QuickJS 2026-06-04.
For another version of quickjs.c, the line ranges must be checked again.

## Regenerate

```bash
tools/quickjs-split/regen.sh
```

This deletes the content of `engine/qjsx/{core,bytecode,builtins,compiler}` before writing.

## Test

The tests need an extracted QuickJS release of the same version
(for `qjs.c`, `qjsc.c`, `quickjs-libc.c`, `repl.js` and `tests/`).

```bash
cd tools/quickjs-split/test
premake5 gmake --cc=gcc --shell=posix --upstream=<release dir> --builddir=<build dir>
make -C <build dir> -j config=release
./run.sh <build dir> <release dir> 2
```

The last argument of `run.sh` is the number of microbench runs per version
(0 or omitted: tests only). The table is written to `<build dir>/bench-table.md`.
