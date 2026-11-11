#!/bin/bash
#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# Writes doc/engine-changes.patch: the exact difference between the split
# engine generated from engine/qjs-bellard (unchanged QuickJS) and engine/qjsx
# (with the hand-made changes described in doc/engine-changes.md).
# Usage: tools/quickjs-split/engine-diff.sh
set -e

TOOL_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_DIR=$(cd "$TOOL_DIR/../.." && pwd)
BASELINE="$PROJECT_DIR/build/qjsx-baseline"
PATCH="$PROJECT_DIR/doc/engine-changes.patch"

mkdir -p "$BASELINE"
find "${BASELINE:?}" -mindepth 1 -delete
bash "$TOOL_DIR/regen.sh" "$BASELINE" > /dev/null

cd "$PROJECT_DIR"
# diff returns 1 when there are differences: not an error here.
diff -ruN --strip-trailing-cr "build/qjsx-baseline" "engine/qjsx" > "$PATCH" || true
echo "written $PATCH ($(grep -c '^@@' "$PATCH") hunks)"
