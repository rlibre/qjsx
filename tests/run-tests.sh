#!/bin/bash
#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# Runs every test. The executables must be built (make -C build config=release).
# Usage: tests/run-tests.sh [Release|Debug|Profile]
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

check "binding library" "$BIN/qjsx-tests" run -A tests/jsbind.test.js
check "typings: the names of every module and global match the runtime" "$BIN/qjsx-tests" run -A tests/types.test.js
check "debugger support of the engine" "$BIN/qjsx-tests" run -A tests/debug.test.js
check "region heap (JS heap of a raw region)" "$BIN/qjsx-tests" run -A tests/region_heap.test.js
check "runtime library" "$BIN/qjsx" run -A tests/runtime.test.js
check "temporal dead zone (let / const checks kept where needed)" "$BIN/qjsx" run -A tests/tdz.test.js
check "tool modules" "$BIN/qjsx" run -A tests/tools.test.js
check "protocol (CBOR, COBS, frames)" "$BIN/qjsx" run -A tests/protocol.test.js
check "debug info (lines, breakpoints, scopes)" "$BIN/qjsx" run -A tests/debuginfo.test.js
check "websocket (sha1, base64, frames, server)" "$BIN/qjsx" run -A tests/websocket.test.js
check "net and serial modules" "$BIN/qjsx" run -A tests/net.test.js
check "process.exit: qjsx run returns its code, nothing runs after it"     bash -c "out=\$('$BIN/qjsx' run tests/fixtures/exit-async.js 2>&1); test \$? -eq 6 && test -z \"\$out\""

# --- package: build the demo project, then run it from the sources and from
# the package (bytecode, like the ESP): the outputs must be identical.
DEMO=tests/fixtures/demo
rm -rf "${DEMO:?}/build"
rm -f "$DEMO/jsconfig.json"
check "build demo" "$BIN/qjsx" build "$DEMO"
check "build writes the typings for VS Code" test -s "$DEMO/.qjsx/types/index.d.ts"
check "build writes a jsconfig.json naming them when the project has none" grep -q '"./.qjsx/types"' "$DEMO/jsconfig.json"
PACKAGE=$(ls "$DEMO"/build/packages/*.qjspkg 2>/dev/null | head -1)
"$BIN/qjsx" run "$DEMO/src/main.js" > build/demo-source.txt 2>&1
"$BIN/qjsx" run "$PACKAGE" > build/demo-package.txt 2>&1
check "package runs like the sources" diff build/demo-source.txt build/demo-package.txt
# The demo imports "stdlib/path": without compiler (like a device), the
# module can only come from the package, where the build copied it.
"$BIN/qjsx-target" run "$PACKAGE" > build/demo-target.txt 2>&1
check "package runs without compiler, with its library module" diff build/demo-source.txt build/demo-target.txt
check "excluded file not packaged" bash -c "! grep -a -q 'ignored.test.js' '$PACKAGE'"

# --- the program made into an executable of the PC (qjsx build --executable:
# qjsx-runtime with the package appended)
check "build --executable" "$BIN/qjsx" build "$DEMO" --executable
DEMO_EXE="$DEMO/build/packages/demo$([ -f "$BIN/qjsx.exe" ] && echo .exe)"
"$DEMO_EXE" > build/demo-executable.txt 2>&1
check "the executable runs like the sources" diff build/demo-source.txt build/demo-executable.txt

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
check "native modules are listed in the manifest" bash -c "grep -a -A 2 '\"natives\"' '$USELIB_PACKAGE' | grep -q '\"sys/crypto\"'"
# A library made by another compiler (another bytecode version) is not
# refused: it holds its sources, the build compiles its modules again and
# says so; the program runs the same.
TESTLIB_FILE="$TESTLIB/build/packages/testlib.qjslib"
"$BIN/qjsx" run -A tests/tamper-package.js "$TESTLIB_FILE" "$TESTLIB_FILE"
rm -rf "${USELIB:?}/build"
check "a library of another bytecode version is compiled again, with a note" \
    bash -c "'$BIN/qjsx' build '$USELIB' 2> build/uselib-recompiled.txt > /dev/null && grep -q \"library 'testlib' .* was made by another compiler .* compiled again\" build/uselib-recompiled.txt"
"$BIN/qjsx-target" run "$(ls "$USELIB"/build/packages/*.qjspkg 2>/dev/null | head -1)" > build/uselib-recompiled-target.txt 2>&1
check "the program built from the recompiled library runs the same" diff --strip-trailing-cr "$USELIB/expected.txt" build/uselib-recompiled-target.txt
rm -rf "${TESTLIB:?}/build" "${USELIB:?}/build"

# --- permissions of a program of the PC (Deno-like, doc/architecture.md
# 6.7): the probe of tests/fixtures/permissions tries files inside its root
# folder, outside it, through "..", through a link (a junction on Windows),
# the network and the environment
PERM=build/perm-test
rm -rf "$PERM" tests/fixtures/permissions/build
mkdir -p "$PERM/perm-root" "$PERM/outside" "$PERM/readonly/src"
echo existing > "$PERM/outside/existing.txt"
EXE_SUFFIX=$([ -f "$BIN/qjsx.exe" ] && echo .exe)
if [ -n "$EXE_SUFFIX" ]; then
    cmd //c "mklink /J build\\perm-test\\perm-root\\link build\\perm-test\\outside" > /dev/null
else
    ln -s "$ROOT/$PERM/outside" "$PERM/perm-root/link"
fi
PERM_OUTSIDE=$(cd "$PERM/outside" && (pwd -W 2> /dev/null || pwd))
PROBE=tests/fixtures/permissions/src/main.js

"$BIN/qjsx" run --qjsx-root-folder="$PERM/perm-root" "$PROBE" "$PERM_OUTSIDE" > build/perm-none.txt 2>&1
check "qjsx run without options: nothing allowed" bash -c "grep -q '^write inside: EACCES' build/perm-none.txt && grep -q '^read inside: EACCES' build/perm-none.txt && grep -q '^listen: EACCES' build/perm-none.txt && grep -qF 'env []' build/perm-none.txt"
check "a refusal says how to grant it" grep -q "^message EACCES: .*read access to .*existing.txt\" is not allowed: grant it with an option of qjsx run" build/perm-none.txt

"$BIN/qjsx" run --qjsx-root-folder="$PERM/perm-root" -R="$PERM/perm-root" -W="$PERM/perm-root" "$PROBE" "$PERM_OUTSIDE" > build/perm-root.txt 2>&1
check "-R / -W: the root folder is process.cwd() and may be used" bash -c "grep -q '^cwd .*/perm-test/perm-root$' build/perm-root.txt && grep -q '^write inside: ok' build/perm-root.txt && grep -q '^read inside: ok' build/perm-root.txt"
check "-R / -W: nothing outside, no escape with .., no way through a link" bash -c "grep -q '^write outside: EACCES' build/perm-root.txt && grep -q '^read outside: EACCES' build/perm-root.txt && grep -q '^escape with ..: EACCES' build/perm-root.txt && grep -q '^read through a link: EACCES' build/perm-root.txt"
check "-R / -W: the parent of the root cannot be removed" bash -c "grep -q '^remove the parent: EACCES' build/perm-root.txt && test -f '$PERM/outside/existing.txt' && ! test -e '$PERM/escaped.txt'"

"$BIN/qjsx" build tests/fixtures/permissions --executable > /dev/null
PERM_PACKAGE=$(ls tests/fixtures/permissions/build/packages/*.qjspkg)
mkdir -p tests/fixtures/permissions/build/packages/perm-root
PERM_TEST=1 OTHER_VARIABLE=2 "$BIN/qjsx" run "$PERM_PACKAGE" "$PERM_OUTSIDE" > build/perm-package.txt 2>&1
check "package: the \"desktop\" section of qjsx.json (root, read, write, env)" bash -c "grep -q '^cwd .*/permissions/build/packages/perm-root$' build/perm-package.txt && grep -q '^write inside: ok' build/perm-package.txt && grep -q '^write outside: EACCES' build/perm-package.txt && grep -qF 'env [\"PERM_TEST\"]' build/perm-package.txt"
"$BIN/qjsx" run --qjsx-root-folder="$PERM/perm-root" "$PERM_PACKAGE" "$PERM_OUTSIDE" > build/perm-option.txt 2>&1
QJSX_ROOT_FOLDER="$PERM/perm-root" "$BIN/qjsx" run "$PERM_PACKAGE" "$PERM_OUTSIDE" > build/perm-variable.txt 2>&1
check "package: the root folder given by --qjsx-root-folder or QJSX_ROOT_FOLDER" bash -c "grep -q '^cwd .*/perm-test/perm-root$' build/perm-option.txt && grep -q '^cwd .*/perm-test/perm-root$' build/perm-variable.txt"
# the root of the disk ("C:/" on Windows: Git Bash would turn "/" into its
# own folder), tried with a script granted nothing: even if the refusal
# failed, the program could not touch anything
DISK_ROOT=$([ -n "$EXE_SUFFIX" ] && echo "C:/" || echo "/")
check "a root folder at the root of a disk is refused" bash -c "! '$BIN/qjsx' run --qjsx-root-folder='$DISK_ROOT' '$PROBE' x > build/perm-disk.txt 2>&1 && grep -q 'cannot be the root of a file system' build/perm-disk.txt && ! grep -q '^cwd' build/perm-disk.txt"
"tests/fixtures/permissions/build/packages/permissions$EXE_SUFFIX" one --qjsx-root-folder="$PERM/perm-root" "$PERM_OUTSIDE" > build/perm-exe.txt 2>&1
check "executable: permissions of its package, --qjsx-root-folder not seen by the program" bash -c "grep -q '^cwd .*/perm-test/perm-root$' build/perm-exe.txt && grep -q '^write inside: ok' build/perm-exe.txt && grep -q '^read outside: EACCES' build/perm-exe.txt && grep -qF 'args [\"one\",' build/perm-exe.txt"

# a root folder without "permissions": read, not write
cp "$PROBE" "$PERM/readonly/src/main.js"
printf '{ "name": "readonly", "desktop": { "rootFolder": "${currentFolder}" } }\n' > "$PERM/readonly/qjsx.json"
"$BIN/qjsx" build "$PERM/readonly" > /dev/null
echo inside > "$PERM/perm-root/inside.txt"
(cd "$PERM/perm-root" && "$BIN/qjsx" run "$(ls "$ROOT/$PERM"/readonly/build/packages/*.qjspkg)" "$PERM_OUTSIDE") > build/perm-readonly.txt 2>&1
check "a root folder alone may be read, not written" bash -c "grep -q '^read inside: ok' build/perm-readonly.txt && grep -q '^write inside: EACCES' build/perm-readonly.txt"
printf '{ "name": "badpath", "desktop": { "permissions": { "write": ["data"] } } }\n' > "$PERM/readonly/qjsx.json"
check "qjsx build refuses a relative path in desktop.permissions" bash -c "! '$BIN/qjsx' build '$PERM/readonly' > build/perm-bad.txt 2>&1 && grep -q 'must start with' build/perm-bad.txt"
rm -rf "$PERM" tests/fixtures/permissions/build

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
"$BIN/qjsx" run -A tests/tamper-package.js "$PACKAGE" build/other-bytecode.qjspkg
check "a package of another bytecode version is refused by the device" \
    bash -c "! '$BIN/qjsx' deploy build/other-bytecode.qjspkg --target '$AGENT_TARGET' > /dev/null 2> build/other-bytecode-deploy.txt && grep -q 'holds bytecode version .* this device reads version' build/other-bytecode-deploy.txt"
check "the installed package stays after the refusal" bash -c "'$BIN/qjsx' device info --target '$AGENT_TARGET' 2>/dev/null | grep -q \"package:  \$(basename '$PACKAGE' .qjspkg | sed 's/^demo-//')\""

# The device gets a reduced package: manifest and bytecode, no debug sidecars
# nor sources (they stay in the full package on the PC).
check "the device gets a reduced package (no debug/, no src/)" \
    bash -c "grep -aq '\"reduced\": true' build/test-agent/app.qjspkg && ! grep -aq 'debug/src/' build/test-agent/app.qjspkg && grep -aq 'bytecode/src/' build/test-agent/app.qjspkg"

# A program that imports a native module the firmware does not have is refused
# by qjsx deploy, before anything is sent (the device lists its native modules
# in its answer to HELLO).
NONATIVE=tests/fixtures/nonative
"$BIN/qjsx" build "$NONATIVE" > /dev/null 2>&1
check "a program needing a missing native module is refused" \
    bash -c "! '$BIN/qjsx' deploy \$(ls '$NONATIVE'/build/packages/*.qjspkg) --target '$AGENT_TARGET' > /dev/null 2> build/nonative-deploy.txt && grep -q 'needs native module(s) device/gpio, which this device does not have' build/nonative-deploy.txt"
check "the installed package stays after that refusal" bash -c "'$BIN/qjsx' device info --target '$AGENT_TARGET' 2>/dev/null | grep -q \"package:  \$(basename '$PACKAGE' .qjspkg | sed 's/^demo-//')\""
rm -rf "${NONATIVE:?}/build"

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
check "device: process.versions has qjsx, quickjs and the build of the package" \
    bash -c "grep '^versions ' build/deviceprocess-out.txt | grep -q '\"qjsx\":\"[0-9.]*\".*\"quickjs\":\".*\"build\":\"$(basename "$DEVICE_PROCESS" .qjspkg | sed 's/^deviceprocess-//')'"
check "device: fs is relative to cwd, the data directory" bash -c "grep -qF 'read back: written by the program' build/deviceprocess-out.txt && grep -qF 'found from cwd: true' build/deviceprocess-out.txt && test -f build/test-agent/data/note.txt"
check "desktop agent: no file outside the data directory" bash -c "grep -qF 'escape: EACCES' build/deviceprocess-out.txt && ! test -e build/test-agent/escaped.txt"
# run on the PC with a path written with '\' (Windows style): argv[1] must
# still come out with '/'
"$BIN/qjsx" run "${DEVICE_PROCESS//\//\\}" > build/deviceprocess-pc.txt 2>&1
check "PC: a package keeps the real argv and env" bash -c "grep -q '^argv .*qjspkg' build/deviceprocess-pc.txt && ! grep -qF 'env {\"GREETING\"' build/deviceprocess-pc.txt"
check "PC: process.argv[0] and argv[1] have '/' separators" bash -c "grep '^argv ' build/deviceprocess-pc.txt | grep -q 'qjsx.*/deviceprocess-' && ! grep '^argv ' build/deviceprocess-pc.txt | grep -qF '\\'"
# made into an executable: argv = [executable, executable, arguments], the
# exit code of process.exit
"$BIN/qjsx" build tests/fixtures/deviceprocess --executable > /dev/null
EXE_SUFFIX=$([ -f "$BIN/qjsx.exe" ] && echo .exe)
"tests/fixtures/deviceprocess/build/packages/deviceprocess$EXE_SUFFIX" one two 2>&1 | grep '^argv ' > build/executable-argv.txt
check "executable: process.argv is [executable, executable, arguments]" \
    bash -c "grep -q 'deviceprocess$EXE_SUFFIX\",\".*deviceprocess$EXE_SUFFIX\",\"one\",\"two\"\]' build/executable-argv.txt"
"$BIN/qjsx" build tests/fixtures/exiting --executable > /dev/null
check "executable: the exit code of process.exit" bash -c "'tests/fixtures/exiting/build/packages/exiting$EXE_SUFFIX' > /dev/null 2>&1; test \$? -eq 3"

# --- a package loaded in RAM (deploy --ram): runs instead of the installed
# one, lost at a restart; deploy --reset restarts the device after installing
# (the desktop agent "restarts" by dropping the RAM package and running the
# installed one again)
EXITING_BUILD=$(basename "$EXITING" .qjspkg | sed 's/^exiting-//')
DEVICE_PROCESS_BUILD=$(basename "$DEVICE_PROCESS" .qjspkg | sed 's/^deviceprocess-//')
"$BIN/qjsx" deploy "$EXITING" --target "$AGENT_TARGET" --ram --monitor --until-exit > build/ram-out.txt 2> build/ram-err.txt
check "deploy --ram runs the package" bash -c "grep -q '^before exit' build/ram-out.txt && grep -q 'loaded .* in RAM' build/ram-err.txt && grep -q 'lost at the next restart' build/ram-err.txt"
check "device info: the package in RAM" bash -c "'$BIN/qjsx' device info --target '$AGENT_TARGET' 2>/dev/null | grep -q \"package:  $EXITING_BUILD.*in RAM\""
check "the package in flash stays" bash -c "grep -aq '$DEVICE_PROCESS_BUILD' build/test-agent/app.qjspkg* 2>/dev/null || grep -raq '$DEVICE_PROCESS_BUILD' build/test-agent"
"$BIN/qjsx" device reset --target "$AGENT_TARGET" 2> /dev/null
sleep 0.5
check "a restart loses the package in RAM" bash -c "'$BIN/qjsx' device info --target '$AGENT_TARGET' 2>/dev/null | grep 'package:' | grep -q \"$DEVICE_PROCESS_BUILD\" && ! '$BIN/qjsx' device info --target '$AGENT_TARGET' 2>/dev/null | grep -q 'in RAM'"
"$BIN/qjsx" deploy "$EXITING" --target "$AGENT_TARGET" --ram --no-run 2> /dev/null
"$BIN/qjsx" deploy "$DEVICE_PROCESS" --target "$AGENT_TARGET" --no-run 2> /dev/null
check "a package installed in flash replaces the one in RAM" bash -c "'$BIN/qjsx' device info --target '$AGENT_TARGET' 2>/dev/null | grep 'package:' | grep -q \"$DEVICE_PROCESS_BUILD\" && ! '$BIN/qjsx' device info --target '$AGENT_TARGET' 2>/dev/null | grep -q 'in RAM'"
check "deploy --ram --reset is refused" \
    bash -c "! '$BIN/qjsx' deploy '$EXITING' --target '$AGENT_TARGET' --ram --reset > /dev/null 2> build/ram-reset.txt && grep -q 'do not go together' build/ram-reset.txt"
"$BIN/qjsx" deploy "$FOREVER" --target "$AGENT_TARGET" --reset > /dev/null 2> build/reset-err.txt
check "deploy --reset restarts the device, the package starts at boot" bash -c "grep -q 'device restarted, connected again' build/reset-err.txt && '$BIN/qjsx' device info --target '$AGENT_TARGET' 2>/dev/null | grep -q 'running:  true'"
"$BIN/qjsx" device stop --target "$AGENT_TARGET" 2> /dev/null

# --- debugger stub, through the agent protocol
check "debugger stub (breakpoints, steps, pause, values)" "$BIN/qjsx" run -A tests/debugger.test.js "$AGENT_TARGET"
check "debugger proxy core (lines, names, evaluation)" "$BIN/qjsx" run -A tests/proxy.test.js "$AGENT_TARGET"
check "debugger CDP server (as js-debug)" "$BIN/qjsx" run -A tests/cdp.test.js "$AGENT_TARGET"

kill "$AGENT_PID" 2> /dev/null
wait "$AGENT_PID" 2> /dev/null
for project in forever failing exiting deviceprocess; do
    rm -rf "tests/fixtures/${project:?}/build"
done

# --- the digital twin of a board: the agent with --board, its page driven
# over HTTP and WebSocket (tests/twin.test.js), its USB device over TCP
# (tests/usb.test.js), its I2C buses (tests/i2c.test.js), its PWM outputs
# (tests/pwm.test.js), its UARTs over TCP and on the page (tests/serial.test.js)
TWIN_PORT=45679
TWIN_UI_PORT=45680
TWIN_USB_PORT=45681
TWIN_UART_PORT=45683
rm -rf build/test-twin
"$BIN/qjsx" agent --board yd-esp32-s3 --port "$TWIN_PORT" --ui-port "$TWIN_UI_PORT" --usb-port "$TWIN_USB_PORT" \
    --uart-port "$TWIN_UART_PORT" --storage build/test-twin > build/twin-out.txt 2>&1 &
TWIN_PID=$!
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    grep -q listening build/twin-out.txt 2>/dev/null && break
    sleep 0.2
done
check "twin agent announces its page" grep -q "twin of YD-ESP32-S3, page on http://127.0.0.1:$TWIN_UI_PORT/" build/twin-out.txt
check "digital twin (board modules, pins, LED, BOOT from the page)" \
    "$BIN/qjsx" run -A tests/twin.test.js "tcp:127.0.0.1:$TWIN_PORT" "$TWIN_UI_PORT"
check "USB device of the twin (device/usb, stdlib/usb, host over TCP)" \
    "$BIN/qjsx" run -A tests/usb.test.js "tcp:127.0.0.1:$TWIN_PORT" "$TWIN_USB_PORT"
check "I2C buses of the twin (device/i2c, nothing answers)" \
    "$BIN/qjsx" run -A tests/i2c.test.js "tcp:127.0.0.1:$TWIN_PORT"
check "PWM outputs of the twin (device/pwm, the limits of the ESP32-S3)" \
    "$BIN/qjsx" run -A tests/pwm.test.js "tcp:127.0.0.1:$TWIN_PORT" "$TWIN_UI_PORT"
check "UARTs of the twin (device/serial, TCP and the page)" \
    "$BIN/qjsx" run -A tests/serial.test.js "tcp:127.0.0.1:$TWIN_PORT" "$TWIN_UART_PORT" "$TWIN_UI_PORT"
kill "$TWIN_PID" 2> /dev/null
wait "$TWIN_PID" 2> /dev/null
rm -rf tests/fixtures/twin/build tests/fixtures/twinusb/build tests/fixtures/twini2c/build \
    tests/fixtures/twinserial/build tests/fixtures/twinpwm/build

# --- the TCP links of the twin chosen from the page (no --uart-port nor
# --usb-port): set, kept in twin.json, taken again after a restart
TWIN_TCP_STORAGE=build/test-twin-tcp
TWIN_TCP_FREE=45690
rm -rf "$TWIN_TCP_STORAGE"
for round in set kept; do
    "$BIN/qjsx" agent --board yd-esp32-s3 --port "$TWIN_PORT" --ui-port "$TWIN_UI_PORT" \
        --storage "$TWIN_TCP_STORAGE" > build/twin-tcp-out.txt 2>&1 &
    TWIN_PID=$!
    for attempt in 1 2 3 4 5 6 7 8 9 10; do
        grep -q listening build/twin-tcp-out.txt 2>/dev/null && break
        sleep 0.2
    done
    check "TCP links of the twin from the page ($round)" \
        "$BIN/qjsx" run -A tests/twintcp.test.js "$round" "$TWIN_UI_PORT" "$TWIN_TCP_STORAGE" "$TWIN_TCP_FREE"
    kill "$TWIN_PID" 2> /dev/null
    wait "$TWIN_PID" 2> /dev/null
done

# --- the UARTs of the twin wired to PC serial ports: "twin.uarts" of a
# project's qjsx.json, the page's choice kept in twin.json; no real port
# needed (the ports do not exist: their errors are checked)
TWINWIRE=tests/fixtures/twinwire
TWINWIRE_PORT=45696        # "twin.ports" of its qjsx.json
TWINWIRE_UI_PORT=45697
rm -rf "$TWINWIRE/build"
"$BIN/qjsx" agent -p "$TWINWIRE" > build/twinwire-out.txt 2>&1 &
TWIN_PID=$!
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    grep -q listening build/twinwire-out.txt 2>/dev/null && break
    sleep 0.2
done
check "UARTs of the twin wired to PC serial ports (qjsx.json, the page, twin.json)" \
    "$BIN/qjsx" run -A tests/twinwire.test.js "tcp:127.0.0.1:$TWINWIRE_PORT" "$TWINWIRE_UI_PORT" "$TWINWIRE/build/agent" 45698
kill "$TWIN_PID" 2> /dev/null
wait "$TWIN_PID" 2> /dev/null
rm -rf "$TWINWIRE/build"
check "--uart-serial refuses the agent's link" \
    bash -c "! '$BIN/qjsx' agent --board yd-esp32-s3 --port 0 --ui-port 0 --storage build/test-twin-wire --uart-serial 0=COM1 > /dev/null 2> build/twinwire-link.txt && grep -q 'agent.s link' build/twinwire-link.txt"
check "--uart-serial refuses what is not <uart>=<PC port>" \
    bash -c "! '$BIN/qjsx' agent --board yd-esp32-s3 --uart-serial COM5 > /dev/null 2> build/twinwire-bad.txt && grep -q 'expected <uart>=<PC port>' build/twinwire-bad.txt"
rm -rf build/test-twin-wire

# --- the twin of a project: "qjsx agent -p" reads the "twin" section of its
# qjsx.json (board, parts, ports); the program reads a BME280 (a model of
# qjsx) and an echo device (a model of the project) over I2C; "target":
# "twin" of the project reaches it
TWINPARTS=tests/fixtures/twinparts
TWINPARTS_PORT=45694       # "twin.ports" of its qjsx.json
TWINPARTS_UI_PORT=45695
rm -rf "$TWINPARTS/build"
"$BIN/qjsx" agent -p "$TWINPARTS" > build/twinparts-out.txt 2>&1 &
TWIN_PID=$!
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    grep -q listening build/twinparts-out.txt 2>/dev/null && break
    sleep 0.2
done
check "twin of a project: its board, ports and storage from qjsx.json" \
    grep -q "listening on 127.0.0.1:$TWINPARTS_PORT, storage $TWINPARTS/build/agent" build/twinparts-out.txt
check "parts of a project on the twin (bme280, a model of the project, I2C, the page)" \
    "$BIN/qjsx" run -A tests/twinparts.test.js "tcp:127.0.0.1:$TWINPARTS_PORT" "$TWINPARTS_UI_PORT"
check '"target": "twin" reaches the twin of the project' \
    bash -c "cd '$TWINPARTS' && '$BIN/qjsx' device info 2>&1 | grep -q 'connected to qjsx twin'"
kill "$TWIN_PID" 2> /dev/null
wait "$TWIN_PID" 2> /dev/null
rm -rf "$TWINPARTS/build"
check "a twin whose parts are wrong does not start" \
    bash -c "! '$BIN/qjsx' agent -p tests/fixtures/twinbad --port 0 --ui-port 0 > /dev/null 2> build/twinbad.txt && grep -q 'another part has this id' build/twinbad.txt"
rm -rf tests/fixtures/twinbad/build

check "an unknown board is refused" bash -c "! '$BIN/qjsx' agent --board no-such-board --port 0 > /dev/null 2> build/twin-unknown.txt && grep -q 'cannot read the board' build/twin-unknown.txt"

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

BARENAME=tests/fixtures/barename
check "a native module imported by a bare name is refused (sys/fs, not fs)" bash -c "! '$BIN/qjsx' build '$BARENAME' > '$ROOT/build/barename.txt' 2>&1 && grep -q \"src/main.js imports 'fs': no module 'fs' (native modules are named sys/<name> or device/<name>\" '$ROOT/build/barename.txt'"
rm -rf "${BARENAME:?}/build"

echo
if [ "$failures" -eq 0 ]; then
    echo "all tests passed"
else
    echo "$failures test(s) failed"
fi
exit "$failures"
