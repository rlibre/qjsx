// Reader of .qjspkg packages: zip archives whose entries are stored without
// compression. The reader does not copy anything: entries point into the
// package memory (a file read in memory on the host, a flash partition
// mapped in memory on the ESP).
//
// Package layout (see doc/architecture.md):
//   manifest.json          { "main": "src/main.js", "scripts": [...], ... }
//   bytecode/<path>.qbc    bytecode of each script / module
//   debug/<path>.json      debug sidecar (not used on the target)
//   src/<path>             copy of the compiled sources
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace package {

struct Entry {
    const char    *name;        // not zero-terminated
    size_t         nameLength;  // bytes in name
    const uint8_t *data;        // stored bytes, inside the package memory
    size_t         size;        // bytes in data
};

class Reader {
public:
    /**
     * No package yet: open() parses one.
     */
    Reader();

    /**
     * Frees the entry table (never the package memory, which the reader
     * does not own).
     */
    ~Reader();
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;

    /**
     * Parses the archive. Returns false and sets error() if it is not a
     * valid package (not a zip, compressed entries, zip64...). 'data' must
     * stay valid as long as the entries are used: they point into it.
     */
    bool         open(const uint8_t *data, size_t size);

    /**
     * The entry with this exact name ("bytecode/src/main.js.qbc"), or
     * nullptr.
     */
    const Entry *find(const char *name) const;

    /**
     * Number of entries.
     */
    int          count() const {
        return _count;
    }

    /**
     * Entry number 'index' (0 <= index < count(), not checked).
     */
    const Entry &entry(int index) const {
        return _entries[index];
    }

    /**
     * Why the last open() failed (static text), or nullptr.
     */
    const char  *error() const {
        return _error;
    }

private:
    /**
     * Records why open() fails and empties the table. Returns false.
     */
    bool         fail(const char *error);

    Entry       *_entries;  // malloc'd; nullptr before open()
    int          _count;    // entries in _entries
    const char  *_error;    // static text; nullptr: no error
};

} // namespace package
