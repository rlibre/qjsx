#!/bin/bash
#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# Writes engine/qjsx/core/rom-atoms.h and rom-atoms.c, the names of the
# engine's built-in objects in read-only memory (QJSX_ROM_ATOMS, quickjs.h).
# Builds generate.c with the engine of the devices (no compiler) as the
# original QuickJS (QJSX_ROM_ATOMS=0, without the old rom-atoms.c), runs it.
# Run it again after a change of the built-in objects of the engine.
# Usage: tools/rom-atoms/generate.sh [output folder, default engine/qjsx/core]
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENGINE="$ROOT/engine/qjsx"
OUTPUT="${1:-$ENGINE/core}"
WORK="$ROOT/build/rom-atoms"
VERSION="$(tr -d '[:space:]' < "$ENGINE/VERSION")"

# 1. compile the engine and the generator, in parallel, as the original
#    QuickJS (the generated files themselves are left out)
rm -rf "$WORK"
mkdir -p "$WORK"
SOURCES=$(ls "$ENGINE"/core/*.c "$ENGINE"/bytecode/*.c "$ENGINE"/builtins/*.c \
    "$ENGINE"/cutils.c "$ENGINE"/dtoa.c "$ENGINE"/libregexp.c "$ENGINE"/libunicode.c \
    "$ROOT/tools/rom-atoms/generate.c" | grep -v "/core/rom-atoms.c$")
FLAGS=(-O1 -std=gnu11 -fwrapv -w -D_GNU_SOURCE -DQJSX_NO_COMPILER -DQJSX_ROM_ATOMS=0
       "-DCONFIG_VERSION=\"$VERSION\"" -I"$ENGINE")
if [ "$OS" = "Windows_NT" ]; then
    FLAGS+=(-D__USE_MINGW_ANSI_STDIO)
fi
for source in $SOURCES; do
    object="$WORK/$(echo "${source#$ROOT/}" | tr '/' '_').o"
    gcc -c "${FLAGS[@]}" "$source" -o "$object" &
done
wait

# 2. link and run it
gcc "$WORK"/*.o -lm -o "$WORK/generate"
"$WORK/generate" "$OUTPUT"
