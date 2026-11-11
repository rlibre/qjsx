#!/bin/bash
#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# Runs the QuickJS tests on qjs-original and qjs-split, and optionally compares their speed.
# Usage: run.sh <build dir> <release dir> [bench runs]
#   <build dir>   the --builddir given to premake (contains bin/qjs-original and bin/qjs-split)
#   <release dir> the --upstream given to premake (contains tests/)
#   [bench runs]  number of microbench runs per version (default 0: tests only)
set -e

BUILD=$1
UPSTREAM=$2
BENCH_RUNS=${3:-0}
TEST_DIR=$(cd "$(dirname "$0")" && pwd)
VERSIONS="original split"

TESTS=(
    "tests/test_closure.js"
    "tests/test_language.js"
    "--std tests/test_builtin.js"
    "tests/test_loop.js"
    "tests/test_bigint.js"
    "tests/test_cyclic_import.js"
    "tests/test_worker.js"
)

cd "$UPSTREAM"
failures=0
for version in $VERSIONS; do
    for test in "${TESTS[@]}"; do
        if "$BUILD/bin/qjs-$version" $test > /dev/null 2>&1; then
            echo "ok    $version  $test"
        else
            echo "FAIL  $version  $test"
            failures=$((failures + 1))
        fi
    done
done
echo "$failures failure(s)"

if [ "$BENCH_RUNS" -gt 0 ]; then
    results=()
    for run in $(seq 1 "$BENCH_RUNS"); do
        for version in $VERSIONS; do
            output="$BUILD/bench-$version-$run.txt"
            "$BUILD/bin/qjs-$version" --std tests/microbench.js > "$output" 2>&1
            echo "bench $version run $run: $(grep total "$output" | awk '{print $2}')"
            results+=("$version=$output")
        done
    done
    node "$TEST_DIR/bench-table.js" "${results[@]}" > "$BUILD/bench-table.md"
    echo "table: $BUILD/bench-table.md"
fi
exit $failures
