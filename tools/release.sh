#!/bin/bash
#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# Releases the version written in common/runtime/version.h, then makes the
# repository ready for the next one. Run from anywhere in the repository:
#
#   tools/release.sh             release it
#   tools/release.sh --dry-run   only say what would be done
#
# What it does, in order (user, 2026-11-19):
#   1. reads the version in version.h, say 0.3.0;
#   2. dates its entry in CHANGELOG.md ("## 0.3.0" becomes "## 0.3.0 -
#      2026-11-19"), commits and pushes;
#   3. tags that commit "v0.3.0" and pushes the tag: GitHub then builds the
#      tool and the firmwares from it and publishes the release
#      (.github/workflows/release.yml);
#   4. raises the last figure in version.h (0.3.1), opens the entry of that
#      version at the top of CHANGELOG.md, without a date, commits and pushes.
# The name of the tag comes from the file, and the file only changes after
# the tag is pushed: the two never disagree, and nobody edits version.h by
# hand. To start a version that is not the next one (0.4.0 after 0.3.x),
# write it in version.h and as the first entry of CHANGELOG.md, commit, then
# work on; this script releases it when its time comes.
#
# It stops, having done nothing, when: the branch is not main; a file of the
# repository is changed and not committed; main is not what GitHub has; the
# version is not three numbers; CHANGELOG.md has no undated entry of the
# version; the tag exists already.
set -eu

VERSION_FILE="common/runtime/version.h"
CHANGELOG="CHANGELOG.md"
BRANCH="main"
REMOTE="origin"

# --- the option
DRY_RUN=0
if [ "${1:-}" = "--dry-run" ]; then
    DRY_RUN=1
elif [ $# -gt 0 ]; then
    echo "usage: tools/release.sh [--dry-run]" >&2
    exit 2
fi

# Stops with a message.
fail() {
    echo "release: $1" >&2
    exit 1
}

# Runs a command, or only prints it with --dry-run.
run() {
    if [ "$DRY_RUN" = 1 ]; then
        echo "  would run: $*"
    else
        "$@"
    fi
}

# --- from the root of the repository
cd "$(git rev-parse --show-toplevel)"

# --- 1. the version of the file: three numbers
version=$(sed -n 's/^#define QJSX_VERSION "\(.*\)"$/\1/p' "$VERSION_FILE")
if ! echo "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    fail "$VERSION_FILE: the version '$version' is not three numbers, as 0.3.0"
fi

tag="v$version"
last=${version##*.}
next="${version%.*}.$((last + 1))"
today=$(date +%Y-%m-%d)

# --- what must hold before anything is done
branch=$(git rev-parse --abbrev-ref HEAD)
if [ "$branch" != "$BRANCH" ]; then
    fail "on the branch '$branch': a release is made from '$BRANCH'"
fi

changed=$(git status --porcelain --untracked-files=no)
if [ -n "$changed" ]; then
    echo "$changed" >&2
    fail "these files are changed and not committed: commit them (or put them back) first"
fi

git fetch --quiet "$REMOTE" "$BRANCH"
if [ "$(git rev-parse HEAD)" != "$(git rev-parse "$REMOTE/$BRANCH")" ]; then
    fail "'$BRANCH' here is not what $REMOTE has: push (or pull) first"
fi

if ! grep -qx "## $version" "$CHANGELOG"; then
    fail "$CHANGELOG has no entry '## $version' without a date: it is the version in progress"
fi

if [ -n "$(git tag --list "$tag")" ] || [ -n "$(git ls-remote --tags "$REMOTE" "refs/tags/$tag")" ]; then
    fail "the tag $tag exists already: $VERSION_FILE must hold the version to release"
fi

echo "release: version $version (tag $tag), dated $today; then $VERSION_FILE goes to $next"

# --- 2. the entry of the version dated, committed and pushed
if [ "$DRY_RUN" = 1 ]; then
    echo "  would write: '## $version - $today' in $CHANGELOG"
else
    sed -i "s/^## $version\$/## $version - $today/" "$CHANGELOG"
fi

run git commit --quiet --message "Release $version" "$CHANGELOG"
run git push "$REMOTE" "$BRANCH"

# --- 3. the tag on that commit, pushed: GitHub builds and publishes
run git tag --annotate "$tag" --message "qjsx $version"
run git push "$REMOTE" "$tag"

# --- 4. the next version: its number in the file, its entry (without a date)
# above the one just released; committed and pushed
if [ "$DRY_RUN" = 1 ]; then
    echo "  would write: #define QJSX_VERSION \"$next\" in $VERSION_FILE"
    echo "  would write: '## $next' above '## $version - $today' in $CHANGELOG"
else
    sed -i "s/^#define QJSX_VERSION \"$version\"\$/#define QJSX_VERSION \"$next\"/" "$VERSION_FILE"
    sed -i "s/^## $version - $today\$/## $next\n\n## $version - $today/" "$CHANGELOG"
fi

run git commit --quiet --message "Version $next (after the release of $version)" "$VERSION_FILE" "$CHANGELOG"
run git push "$REMOTE" "$BRANCH"

echo "release: done"
