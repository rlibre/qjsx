/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

#include "package.h"
#include "memory.h"

#include <stdlib.h>
#include <string.h>

namespace package {

namespace {

// Zip records (little endian), see PKWARE APPNOTE.TXT.
const uint32_t END_OF_CENTRAL_DIRECTORY = 0x06054b50;
const uint32_t CENTRAL_DIRECTORY_HEADER = 0x02014b50;
const uint32_t LOCAL_FILE_HEADER = 0x04034b50;
const size_t END_RECORD_SIZE = 22;
const size_t CENTRAL_HEADER_SIZE = 46;
const size_t LOCAL_HEADER_SIZE = 30;
const uint16_t METHOD_STORED = 0;

/**
 * Little-endian 16-bit value (any alignment).
 */
uint16_t read16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

/**
 * Little-endian 32-bit value (any alignment).
 */
uint32_t read32(const uint8_t *p) {
    return (uint32_t)(p[0]) | (uint32_t)(p[1]) << 8
         | (uint32_t)(p[2]) << 16 | (uint32_t)(p[3]) << 24;
}

/**
 * The end of central directory record of the archive, or nullptr if absent.
 */
const uint8_t *findEndRecord(const uint8_t *data, size_t size) {
    // too small to hold one
    if (size < END_RECORD_SIZE) {
        return nullptr;
    }

    // the record is at the end, possibly followed by a comment (<= 65535
    // bytes): search its signature backwards, from the last possible
    // position down to the farthest one a comment allows
    size_t lowest = size > END_RECORD_SIZE + 65535 ? size - END_RECORD_SIZE - 65535 : 0;

    for (size_t offset = size - END_RECORD_SIZE + 1; offset-- > lowest;) {
        if (read32(data + offset) == END_OF_CENTRAL_DIRECTORY) {
            return data + offset;
        }
    }

    return nullptr;
}

} // namespace

/**
 * Empty reader.
 */
Reader::Reader() : _entries(nullptr), _count(0), _error(nullptr) {}

/**
 * Frees the entry table only.
 */
Reader::~Reader() {
    memory::release(_entries);
}

namespace {

/**
 * True if [start, start + length) lies inside [0, limit). Never overflows: the
 * values come from the archive and can be anything.
 */
bool within(uint64_t start, uint64_t length, uint64_t limit) {
    return start <= limit && length <= limit - start;
}

} // namespace

/**
 * Records why open() fails and empties the table.
 */
bool Reader::fail(const char *error) {
    memory::release(_entries);
    _entries = nullptr;
    _count = 0;
    _error = error;
    return false;
}

/**
 * Parses the central directory of the package into the entry table; on any
 * inconsistency the table stays empty and error() says why.
 */
bool Reader::open(const uint8_t *data, size_t size) {
    // forget a previous package
    memory::release(_entries);
    _entries = nullptr;
    _count = 0;
    _error = nullptr;

    // find the end record
    const uint8_t *end = findEndRecord(data, size);
    if (end == nullptr) {
        return fail("not a zip archive");
    }

    // read from it the entry count and the place of the central directory,
    // which must be inside the archive (0xffffffff means zip64: refused)
    uint16_t entryCount = read16(end + 10);
    uint32_t directorySize = read32(end + 12);
    uint32_t directoryOffset = read32(end + 16);
    if (directoryOffset == 0xffffffff || !within(directoryOffset, directorySize, size)) {
        return fail("invalid central directory (zip64 is not supported)");
    }

    // allocate the entry table
    _entries = (Entry *)memory::allocate(sizeof(Entry) * (entryCount > 0 ? entryCount : 1));

    // walk the central directory, one header per entry; positions are offsets
    // in the archive, computed on 64 bits
    uint64_t position = directoryOffset;
    uint64_t directoryEnd = (uint64_t)directoryOffset + directorySize;

    for (int i = 0; i < entryCount; i++) {
        // the header must fit in the directory and carry its signature (the
        // pointer is made once the offset is known to be inside)
        if (!within(position, CENTRAL_HEADER_SIZE, directoryEnd)) {
            return fail("invalid central directory entry");
        }

        const uint8_t *header = data + position;
        if (read32(header) != CENTRAL_DIRECTORY_HEADER) {
            return fail("invalid central directory entry");
        }

        // read its fields; only stored (uncompressed) entries are accepted
        uint16_t method = read16(header + 10);
        uint32_t compressedSize = read32(header + 20);
        uint32_t uncompressedSize = read32(header + 24);
        uint16_t nameLength = read16(header + 28);
        uint16_t extraLength = read16(header + 30);
        uint16_t commentLength = read16(header + 32);
        uint32_t localOffset = read32(header + 42);
        if (method != METHOD_STORED || compressedSize != uncompressedSize) {
            return fail("compressed entries are not supported (entries must be stored)");
        }

        // its name follows the header and must be in the directory too
        uint64_t namePosition = position + CENTRAL_HEADER_SIZE;
        if (!within(namePosition, nameLength, directoryEnd)) {
            return fail("invalid central directory entry");
        }

        // the local header it points to must be in the archive and carry its
        // signature
        if (!within(localOffset, LOCAL_HEADER_SIZE, size)) {
            return fail("invalid local file header");
        }

        const uint8_t *local = data + localOffset;
        if (read32(local) != LOCAL_FILE_HEADER) {
            return fail("invalid local file header");
        }

        // the data starts after the local header, its name and its extra
        // field (whose lengths may differ from the central ones), and must
        // be inside the archive
        uint64_t dataOffset = (uint64_t)localOffset + LOCAL_HEADER_SIZE + read16(local + 26) + read16(local + 28);
        if (!within(dataOffset, compressedSize, size)) {
            return fail("entry outside of the archive");
        }

        // record the entry, go to the next header
        _entries[_count++] = Entry{
            (const char *)(data + namePosition), nameLength,
            data + dataOffset, compressedSize,
        };
        position = namePosition + nameLength + extraLength + commentLength;
    }

    return true;
}

/**
 * The entry with this exact name, or nullptr (linear search).
 */
const Entry *Reader::find(const char *name) const {
    // compare the lengths first, then the bytes: entry names are not
    // zero-terminated
    size_t length = strlen(name);

    for (int i = 0; i < _count; i++) {
        bool same = _entries[i].nameLength == length && memcmp(_entries[i].name, name, length) == 0;
        if (same) {
            return &_entries[i];
        }
    }

    return nullptr;
}

} // namespace package
