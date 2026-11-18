/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Images of the renderer (image.h, which describes the format): the header
// and the rows of a file, read in place. The runs are drawn by draw.cpp.
#include "image.h"

#include <string.h>

namespace draw {

namespace {

const uint8_t IMAGE_MAGIC[4] = { 'Q', 'I', 'M', 'G' };
const int     IMAGE_VERSION = 1;
const size_t  IMAGE_HEADER_BYTES = 12;
const size_t  ROW_OFFSET_BYTES = 4;

/**
 * The little endian value of 2 bytes at 'at'.
 */
uint32_t read16(const uint8_t *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8);
}

/**
 * The little endian value of 4 bytes at 'at'.
 */
uint32_t read32(const uint8_t *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

} // namespace

bool openImage(const uint8_t *data, size_t size, Image *out) {
    // the header: this format, this version
    bool hasHeader = size >= IMAGE_HEADER_BYTES && memcmp(data, IMAGE_MAGIC, sizeof(IMAGE_MAGIC)) == 0;
    if (!hasHeader || data[4] != IMAGE_VERSION) {
        return false;
    }

    // a size, and the offsets of its rows in the file
    int width = (int)read16(data + 6);
    int height = (int)read16(data + 8);
    bool hasRows = width >= 1 && height >= 1 && IMAGE_HEADER_BYTES + (size_t)height * ROW_OFFSET_BYTES <= size;
    if (!hasRows) {
        return false;
    }

    *out = Image{ data, size, width, height };
    return true;
}

bool imageRow(const Image *image, int row, const uint8_t **start, const uint8_t **end) {
    // where it starts, and where the next one does (the last: the end of the file)
    const uint8_t *offsets = image->data + IMAGE_HEADER_BYTES;
    size_t first = read32(offsets + (size_t)row * ROW_OFFSET_BYTES);
    size_t last = row + 1 < image->height ? read32(offsets + (size_t)(row + 1) * ROW_OFFSET_BYTES) : image->size;

    // within the file, after the table of the rows
    size_t dataStart = IMAGE_HEADER_BYTES + (size_t)image->height * ROW_OFFSET_BYTES;
    bool isInFile = first >= dataStart && first <= last && last <= image->size;
    if (!isInFile) {
        return false;
    }

    *start = image->data + first;
    *end = image->data + last;
    return true;
}

} // namespace draw
