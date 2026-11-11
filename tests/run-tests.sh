#!/bin/bash
#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# Runs every test. The executables must be built (make -C build config=release).
# Usage: tests/run-tests.sh [Release|Debug]
set -u

CONFIG=${1:-Release}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="$ROOT/bin/$CONFIG"
OUTPUT="$ROOT/build/test-output.txt"
failures=0

cd "$ROOT" || exit 1
mkdir -p build

# check <name> <command...>: runs the command (60 s at most) and reports.
check() {
    local name=$1
    shift
    if timeout 60 "$@" > "$OUTPUT" 2>&1; then
        echo "ok    $name"
    else
        echo "FAIL  $name"
        sed 's/^/      /' "$OUTPUT"
        failures=$((failures + 1))
    fi
}

check "binding library" "$BIN/qjsx-tests" run tests/jsbind.test.js
check "debugger support of the engine" "$BIN/qjsx-tests" run tests/debug.test.js
check "runtime library" "$BIN/qjsx" run tests/runtime.test.js
check "tool modules" "$BIN/qjsx" run tests/tools.test.js
check "protocol (CBOR, COBS, frames)" "$BIN/qjsx" run tests/protocol.test.js
check "debug info (lines, breakpoints, scopes)" "$BIN/qjsx" run tests/debuginfo.test.js
check "websocket (sha1, base64, frames, server)" "$BIN/qjsx" run tests/websocket.test.js
check "net and serial modules" "$BIN/qjsx" run tests/net.test.js
check "process.exit: qjsx run returns its code, nothing runs after it"     bash -c "out=\$('$BIN/qjsx' run tests/fixtures/exit-async.js 2>&1); test \$? -eq 6 && test -z \"\$out\""

# --- package: build the demo project, then run it from the sources and from
# the package (bytecode, like the ESP): the outputs must be identical.
DEMO=tests/fixtures/demo
rm -rf "${DEMO:?}/build"
check "build demo" "$BIN/qjsx" build "$DEMO"
PACKAGE=$(ls "$DEMO"/build/packages/*.qjspkg 2>/dev/null | head -1)
"$BIN/qjsx" run "$DEMO/src/main.js" > build/demo-source.txt 2>&1
"$BIN/qjsx" run "$PACKAGE" > build/demo-package.txt 2>&1
check "package runs like the sources" diff build/demo-source.txt build/demo-package.txt
# The demo imports "stdlib/path": without compiler (like a device), the
# module can only come from the package, where the build copied it.
"$BIN/qjsx-target" run "$PACKAGE" > build/demo-target.txt 2>&1
check "package runs without compiler, with its library module" diff build/demo-source.txt build/demo-target.txt
check "excluded file not packaged" bash -c "! grep -a -q 'ignored.test.js' '$PACKAGE'"

# The package is a regular zip: an external tool must read it.
if command -v unzip > /dev/null; then
    check "package is a valid zip" unzip -tq "$PACKAGE"
elif tar --version 2>/dev/null | grep -q bsdtar; then
    check "package is a valid zip" tar -tf "$PACKAGE"
fi

# --- engine without compiler (like the ESP32 / RP2350 targets): same output
# as with the full engine, except eval which is not supported.
NOCOMPILER=tests/fixtures/nocompiler
rm -rf "${NOCOMPILER:?}/build"
check "eval() gives a warning with its position, the build still succeeds" \
    bash -c "'$BIN/qjsx' build '$NOCOMPILER' 2> build/nocompiler-build.txt > /dev/null && grep -q 'src/main.js:53:[0-9]*: warning: eval() needs the compiler' build/nocompiler-build.txt"
NOCOMPILER_PACKAGE=$(ls "$NOCOMPILER"/build/packages/*.qjspkg)
"$BIN/qjsx" run "$NOCOMPILER_PACKAGE" > build/nocompiler-full.txt 2>&1
"$BIN/qjsx-target" run "$NOCOMPILER_PACKAGE" > build/nocompiler-target.txt 2>&1
check "engine without compiler runs a package like the full one" \
    diff <(grep -v '^eval' build/nocompiler-full.txt) <(grep -v '^eval' build/nocompiler-target.txt)
check "engine without compiler has no eval" grep -q '^eval TypeError: eval is not supported' build/nocompiler-target.txt
rm -rf "${NOCOMPILER:?}/build"

# Rebuilding the same sources gives the same build id (same file name).
check "build is reproducible" bash -c "'$BIN/qjsx' build '$DEMO' > /dev/null && test \$(ls '$DEMO'/build/packages/*.qjspkg | wc -l) -eq 1"

# --- libraries (.qjslib): the library modules a program imports are copied
# into its package, each one whole, and nothing else of the library.
TESTLIB=tests/fixtures/testlib
USELIB=tests/fixtures/uselib
rm -rf "${TESTLIB:?}/build" "${USELIB:?}/build"
check "the standard library is next to the executable" test -f "$BIN/stdlib.qjslib"
check "mklib builds a library" "$BIN/qjsx" mklib "$TESTLIB"
check "build a program that uses libraries" "$BIN/qjsx" build "$USELIB"
USELIB_PACKAGE=$(ls "$USELIB"/build/packages/*.qjspkg 2>/dev/null | head -1)
"$BIN/qjsx-target" run "$USELIB_PACKAGE" > build/uselib-target.txt 2>&1
check "library modules run from the package, without compiler" diff --strip-trailing-cr "$USELIB/expected.txt" build/uselib-target.txt
check "imported library modules are packaged, followed through the libraries" \
    bash -c "grep -a -q 'bytecode/testlib/greet.qbc' '$USELIB_PACKAGE' && grep -a -q 'bytecode/testlib/words.qbc' '$USELIB_PACKAGE' && grep -a -q 'bytecode/stdlib/path.qbc' '$USELIB_PACKAGE'"
check "a library module nobody imports is not packaged" bash -c "! grep -a -q 'testlib/unused' '$USELIB_PACKAGE'"
check "native modules are listed in the manifest" bash -c "grep -a -A 2 '\"natives\"' '$USELIB_PACKAGE' | grep -q '\"crypto\"'"
# A library made by another compiler (another bytecode version) is not
# refused: it holds its sources, the build compiles its modules again and
# says so; the program runs the same.
TESTLIB_FILE="$TESTLIB/build/packages/testlib.qjslib"
"$BIN/qjsx" run tests/tamper-package.js "$TESTLIB_FILE" "$TESTLIB_FILE"
rm -rf "${USELIB:?}/build"
check "a library of another bytecode version is compiled again, with a note" \
    bash -c "'$BIN/qjsx' build '$USELIB' 2> build/uselib-recompiled.txt > /dev/null && grep -q \"library 'testlib' .* was made by another compiler .* compiled again\" build/uselib-recompiled.txt"
"$BIN/qjsx-target" run "$(ls "$USELIB"/build/packages/*.qjspkg 2>/dev/null | head -1)" > build/uselib-recompiled-target.txt 2>&1
check "the program built from the recompiled library runs the same" diff --strip-trailing-cr "$USELIB/expected.txt" build/uselib-recompiled-target.txt
rm -rf "${TESTLIB:?}/build" "${USELIB:?}/build"

# --- device agent: the desktop agent stands for an ESP board.
AGENT_PORT=45678
AGENT_TARGET="tcp:127.0.0.1:$AGENT_PORT"
rm -rf build/test-agent
"$BIN/qjsx" agent --port "$AGENT_PORT" --storage build/test-agent > build/agent-out.txt 2>&1 &
AGENT_PID=$!
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    grep -q listening build/agent-out.txt 2>/dev/null && break
    sleep 0.2
done

"$BIN/qjsx" deploy "$PACKAGE" --target "$AGENT_TARGET" --monitor --until-exit > build/deploy-out.txt 2> build/deploy-err.txt
check "deploy + monitor gives the program output" diff build/demo-source.txt build/deploy-out.txt
check "device info shows the installed build" bash -c "'$BIN/qjsx' device info --target '$AGENT_TARGET' 2>/dev/null | grep -q \"package:  \$(basename '$PACKAGE' .qjspkg | sed 's/^demo-//')\""

# A package compiled for another engine (another bytecode version) is refused:
# by the device before anything is written, so that the installed package
# stays, and by the loader when the package is run.
"$BIN/qjsx" run tests/tamper-package.js "$PACKAGE" build/other-bytecode.qjspkg
check "a package of another bytecode version is refused by the device" \
    bash -c "! '$BIN/qjsx' deploy build/other-bytecode.qjspkg --target '$AGENT_TARGET' > /dev/null 2> build/other-bytecode-deploy.txt && grep -q 'holds bytecode version .* this device reads version' build/other-bytecode-deploy.txt"
check "the installed package stays after the refusal" bash -c "'$BIN/qjsx' device info --target '$AGENT_TARGET' 2>/dev/null | grep -q \"package:  \$(basename '$PACKAGE' .qjspkg | sed 's/^demo-//')\""
check "a package of another bytecode version does not run" \
    bash -c "! '$BIN/qjsx' run build/other-bytecode.qjspkg > build/other-bytecode-run.txt 2>&1 && grep -q 'holds bytecode version .* this engine reads version' build/other-bytecode-run.txt"

for project in forever failing exiting deviceprocess; do
    rm -rf "tests/fixtures/${project:?}/build"
    "$BIN/qjsx" build "tests/fixtures/$project" > /dev/null
done
FOREVER=$(ls tests/fixtures/forever/build/packages/*.qjspkg)
"$BIN/qjsx" deploy "$FOREVER" --target "$AGENT_TARGET" 2> /dev/null
check "a program is running" bash -c "'$BIN/qjsx' device info --target '$AGENT_TARGET' 2>/dev/null | grep -q 'running:  true'"
"$BIN/qjsx" device stop --target "$AGENT_TARGET" 2> /dev/null
sleep 0.3
check "device stop ends it" bash -c "'$BIN/qjsx' device info --target '$AGENT_TARGET' 2>/dev/null | grep -q 'running:  false'"

FAILING=$(ls tests/fixtures/failing/build/packages/*.qjspkg)
"$BIN/qjsx" deploy "$FAILING" --target "$AGENT_TARGET" --monitor --until-exit > build/failing-out.txt 2> build/failing-err.txt
check "stderr of the program is forwarded" grep -q "^to stderr" build/failing-err.txt
check "uncaught exception is reported" grep -q "Uncaught Error: boom" build/failing-err.txt
check "program goes on after an uncaught exception" grep -q "still running after the exception" build/failing-out.txt

EXITING=$(ls tests/fixtures/exiting/build/packages/*.qjspkg)
"$BIN/qjsx" deploy "$EXITING" --target "$AGENT_TARGET" --monitor --until-exit > build/exiting-out.txt 2> build/exiting-err.txt
check "process.exit stops the program at once" bash -c "grep -q '^before exit' build/exiting-out.txt && ! grep -q 'should not run' build/exiting-out.txt"
check "process.exit code reaches the host" grep -q "program ended (code 3)" build/exiting-err.txt
check "the agent survives process.exit" bash -c "'$BIN/qjsx' device info --target '$AGENT_TARGET' 2>/dev/null | grep -q 'running:  false'"

# deployed over a running program (forever): its stop is silent, and
# --until-exit waits for the end of the new program, not of the stopped one
"$BIN/qjsx" deploy "$FOREVER" --target "$AGENT_TARGET" 2> /dev/null
DEVICE_PROCESS=$(ls tests/fixtures/deviceprocess/build/packages/*.qjspkg)
"$BIN/qjsx" deploy "$DEVICE_PROCESS" --target "$AGENT_TARGET" --monitor --until-exit > build/deviceprocess-out.txt 2> build/deviceprocess-err.txt
check "stopping a running program prints no error" bash -c "! grep -q 'interrupted' build/deviceprocess-out.txt build/deviceprocess-err.txt"
check "deploy --until-exit waits for the new program" grep -qF 'program started (build' build/deviceprocess-err.txt
check "device: process.argv is qjsx + main module" grep -qF 'argv ["qjsx","src/main.js"]' build/deviceprocess-out.txt
check "device: process.env comes from qjsx.json" grep -qF 'env {"GREETING":"hello","MODE":"test"}' build/deviceprocess-out.txt
check "device: fs is relative to cwd, the data directory" bash -c "grep -qF 'read back: written by the program' build/deviceprocess-out.txt && grep -qF 'found from cwd: true' build/deviceprocess-out.txt && test -f build/test-agent/data/note.txt"
# run on the PC with a path written with '\' (Windows style): argv[1] must
# still come out with '/'
"$BIN/qjsx" run "${DEVICE_PROCESS//\//\\}" > build/deviceprocess-pc.txt 2>&1
check "PC: a package keeps the real argv and env" bash -c "grep -q '^argv .*qjspkg' build/deviceprocess-pc.txt && ! grep -qF 'env {\"GREETING\"' build/deviceprocess-pc.txt"
check "PC: process.argv[0] and argv[1] have '/' separators" bash -c "grep '^argv ' build/deviceprocess-pc.txt | grep -q 'qjsx.*/deviceprocess-' && ! grep '^argv ' build/deviceprocess-pc.txt | grep -qF '\\'"

# --- debugger stub, through the agent protocol
check "debugger stub (breakpoints, steps, pause, values)" "$BIN/qjsx" run tests/debugger.test.js "$AGENT_TARGET"
check "debugger proxy core (lines, names, evaluation)" "$BIN/qjsx" run tests/proxy.test.js "$AGENT_TARGET"
check "debugger CDP server (as js-debug)" "$BIN/qjsx" run tests/cdp.test.js "$AGENT_TARGET"

kill "$AGENT_PID" 2> /dev/null
wait "$AGENT_PID" 2> /dev/null
for project in forever failing exiting deviceprocess; do
    rm -rf "tests/fixtures/${project:?}/build"
done

# --- a syntax error is reported with its position.
BROKEN=tests/fixtures/broken
check "syntax error reported" bash -c "! '$BIN/qjsx' build '$BROKEN' > '$ROOT/build/broken.txt' 2>&1 && grep -q 'src/main.js:10' '$ROOT/build/broken.txt'"
rm -rf "${BROKEN:?}/build"

# --- what a build refuses: 'import()' (every module must be known at build
# time), reported with its position, and a module of an undeclared library.
DYNAMIC=tests/fixtures/dynamicimport
check "import() refused with its position" bash -c "! '$BIN/qjsx' build '$DYNAMIC' > '$ROOT/build/dynamicimport.txt' 2>&1 && grep -q \"'import()' is not allowed\" '$ROOT/build/dynamicimport.txt' && grep -q 'src/main.js:10' '$ROOT/build/dynamicimport.txt'"
rm -rf "${DYNAMIC:?}/build"
NOLIBRARY=tests/fixtures/nolibrary
check "module of an undeclared library refused" bash -c "! '$BIN/qjsx' build '$NOLIBRARY' > '$ROOT/build/nolibrary.txt' 2>&1 && grep -q \"src/main.js imports 'nolib/thing'.*no library 'nolib' is declared\" '$ROOT/build/nolibrary.txt'"
rm -rf "${NOLIBRARY:?}/build"

echo
if [ "$failures" -eq 0 ]; then
    echo "all tests passed"
else
    echo "$failures test(s) failed"
fi
exit "$failures"
