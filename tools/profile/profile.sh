#!/bin/bash
#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# Measures of the engine on the PC (Windows): runs a measure script with
# qjsx-tests (default tools/profile/bench.js: the cost of each kind of call,
# then the tests), prints what the script prints, then for each of its
# profiles the functions where the JS thread was found, with their share of
# the samples (addresses turned into names by addr2line: the innermost
# function when code was inlined).
#   bash tools/profile/profile.sh [Profile|Debug] [lines per profile, default 12] [script]
# e.g. tools/profile/workload-profiled.js for the heavier workload. Run from
# the root of the repository, with nothing else heavy running. Profile is the
# code of Release with its debug info (make -C build config=profile): Release
# itself has none, addr2line could not name its functions.
set -e
CONFIG=${1:-Profile}
TOP=${2:-12}
SCRIPT=${3:-tools/profile/bench.js}
EXE=bin/$CONFIG/qjsx-tests.exe
RAW=build/profile-raw.txt

# the measures (a few minutes at most)
timeout 600 "$EXE" run "$SCRIPT" > "$RAW"

# 1. what the script printed itself
grep -v -E '^(profile|sample|samples|base) ' "$RAW"

# the base of the executable file, and where it was loaded (the samples are
# addresses in memory: moved by the difference for addr2line)
FILE_BASE=$(objdump -p "$EXE" | awk '$1 == "ImageBase" { print $2 }')
LOADED_BASE=$(grep -m1 '^base ' "$RAW" | cut -d' ' -f2)

# 2. each profile: resolve its addresses, add up by function, best first
grep '^profile ' "$RAW" | cut -c9- | while read -r name; do
    echo
    echo "== $name"

    # the sample lines of this profile: from its line to the next profile
    awk -v want="profile $name" '
        $0 == want { inside = 1; next }
        /^profile / { inside = 0 }
        inside && /^sample / { print $2, $3 }
    ' "$RAW" > build/profile-samples.txt

    # the function of each address (moved to the file's base), then the
    # counts added up by function
    while read -r address count; do
        printf '0x%x\n' $((0x$address - 0x$LOADED_BASE + 0x$FILE_BASE))
    done < build/profile-samples.txt \
        | addr2line -f -C -e "$EXE" | awk 'NR % 2 == 1' > build/profile-names.txt
    paste -d' ' build/profile-samples.txt build/profile-names.txt | awk '
        { count = $2; $1 = ""; $2 = ""; sub(/^  /, ""); total += count; byName[$0] += count }
        END { for (name in byName) printf "%6.1f %%  %s\n", 100 * byName[name] / total, name }
    ' | sort -rn | head -n "$TOP"
done
