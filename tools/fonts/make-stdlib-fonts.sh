#!/bin/bash
#          _
#  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
# / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
# \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
#    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)

# Makes the font files of the standard library (lib/stdlib/src/fonts/*.bin,
# imported as bytes by the modules stdlib/fonts/<name>) from Inter Regular
# (tools/fonts/inter/Inter-Regular.ttf, SIL Open Font License 1.1, its text in
# tools/fonts/inter/LICENSE.txt and lib/stdlib/src/fonts/LICENSE-Inter.txt).
# Converter: lv_font_conv, installed in tools/fonts (npm install there); the
# format is LVGL's binary font format, 4 bits per pixel, WITHOUT compression
# (the renderer reads the glyphs where they lie, band by band) and without
# kerning. Rebuild qjsx afterwards (stdlib.qjslib and the embedded copy).
# Usage: tools/fonts/make-stdlib-fonts.sh
set -e

TOOLS="$(cd "$(dirname "$0")" && pwd)"
FONT="$TOOLS/inter/Inter-Regular.ttf"
OUTPUT="$TOOLS/../../lib/stdlib/src/fonts"

# Character sets: printable ASCII, then the printable part of Latin-1
# (accents, the degree sign...)
ASCII="0x20-0x7E"
LATIN1="0xA0-0xFF"
DEGREE="0xB0"

# One font file: make <size> <file name> <ranges...>
make() {
    local size="$1"
    local name="$2"
    shift 2

    local ranges=()
    for range in "$@"; do
        ranges+=(-r "$range")
    done

    (cd "$TOOLS" && npx --no-install lv_font_conv --font "$FONT" --size "$size" --bpp 4 --format bin \
        --no-compress --no-kerning "${ranges[@]}" -o "$OUTPUT/$name")
    echo "$name: $(wc -c < "$OUTPUT/$name") bytes"
}

# text: ASCII and Latin-1
make 16 inter16.bin "$ASCII" "$LATIN1"
make 24 inter24.bin "$ASCII" "$LATIN1"

# values: ASCII and the degree sign only (Latin-1 would double its size)
make 40 inter40.bin "$ASCII" "$DEGREE"
