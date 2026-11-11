#!/bin/bash
# Regenerates engine/qjsx from engine/qjs-bellard (quickjs.c and the other engine files).
# Usage: tools/quickjs-split/regen.sh [--force] [output directory]
#
# Since the engine changes of the debugger (doc/engine-changes.md),
# engine/qjsx is edited by hand: regenerating it would erase them.
# Without --force, only another output directory is accepted (that is what
# engine-diff.sh does to compare).
set -e

TOOL_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_DIR=$(cd "$TOOL_DIR/../.." && pwd)
ORIGINAL="$PROJECT_DIR/engine/qjs-bellard"
SPLIT="$PROJECT_DIR/engine/qjsx"

FORCE=0
if [ "${1:-}" = "--force" ]; then
    FORCE=1
    shift
fi
if [ -n "${1:-}" ]; then
    SPLIT=$1
elif [ "$FORCE" -eq 0 ]; then
    echo "regen.sh: engine/qjsx contains hand-made engine changes (doc/engine-changes.md)." >&2
    echo "regen.sh: give another output directory, or --force to erase them." >&2
    exit 1
fi

# files of the engine copied unchanged
SUPPORT_FILES="cutils.c cutils.h dtoa.c dtoa.h libregexp.c libregexp.h libregexp-opcode.h
               libunicode.c libunicode.h libunicode-table.h list.h quickjs.h quickjs-atom.h
               quickjs-opcode.h LICENSE VERSION"

# remove the previously generated sources (only the generated folders)
for dir in core bytecode builtins compiler; do
    if [ -d "${SPLIT:?}/$dir" ]; then
        find "${SPLIT:?}/$dir" -mindepth 1 -delete
    fi
done

mkdir -p "$SPLIT"
node "$TOOL_DIR/split.js" "$ORIGINAL/quickjs.c" "$SPLIT"

for file in $SUPPORT_FILES; do
    cp "$ORIGINAL/$file" "$SPLIT/$file"
done
echo "regenerated $SPLIT"
