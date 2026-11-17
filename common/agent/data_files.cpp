/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The files of the device's data folder, for the host (see data_files.h).
#include "data_files.h"
#include "files.h"
#include "memory.h"

#include <errno.h>
#include <string.h>

namespace agent {

namespace {

const char TEMPORARY_SUFFIX[] = ".qjsx-part";   // the file being written, renamed once complete
const size_t MAX_PATH = 512;                    // bytes of a resolved path

/**
 * True when 'name' ends with the suffix of a temporary file.
 */
bool isTemporary(const char *name) {
    size_t length = strlen(name);
    size_t suffix = sizeof(TEMPORARY_SUFFIX) - 1;
    return length > suffix && strcmp(name + length - suffix, TEMPORARY_SUFFIX) == 0;
}

// What list() gathers, entry by entry.
struct Listing {
    const char *folder;     // the absolute folder listed
    FileEntry  *entries;    // malloc'd, grown as needed
    int         count;      // entries used
    int         capacity;   // entries allocated
};

/**
 * listDirectory visitor: adds 'name' with its size and type (temporary
 * files left out).
 */
bool addEntry(void *opaque, const char *name) {
    // the temporary file of an unfinished write is not shown
    Listing *listing = (Listing *)opaque;
    if (isTemporary(name)) {
        return true;
    }

    // room for one more
    if (listing->count == listing->capacity) {
        listing->capacity = listing->capacity == 0 ? 16 : listing->capacity * 2;
        listing->entries = (FileEntry *)memory::reallocate(listing->entries, sizeof(FileEntry) * listing->capacity);
    }

    // its size and type
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", listing->folder, name);
    files::Status status = {};
    files::stat(path, &status);

    // the entry, with a copy of its name
    FileEntry *entry = &listing->entries[listing->count++];
    entry->name = memory::copyText(name, strlen(name));
    entry->isDirectory = status.isDirectory;
    entry->size = status.isDirectory ? 0 : status.size;
    return true;
}

// What usedBytes() adds up, folder by folder.
struct Usage {
    const char *folder;   // the absolute folder being visited
    uint64_t    used;     // bytes so far
};

/**
 * Defined below: addFolderUsage() and addUsage() call each other.
 */
bool addUsage(void *opaque, const char *name);

/**
 * Adds the sizes of every file under 'folder' to *used.
 */
void addFolderUsage(const char *folder, uint64_t *used) {
    Usage usage = { folder, *used };
    files::listDirectory(folder, &addUsage, &usage);
    *used = usage.used;
}

/**
 * listDirectory visitor of usedBytes(): a file adds its size, a folder its
 * content.
 */
bool addUsage(void *opaque, const char *name) {
    Usage *usage = (Usage *)opaque;

    // its type and size; an entry that vanished meanwhile counts nothing
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", usage->folder, name);
    files::Status status = {};
    if (!files::stat(path, &status)) {
        return true;
    }

    // a folder: its content, recursively; a file: its size
    if (status.isDirectory) {
        addFolderUsage(path, &usage->used);
    } else {
        usage->used += (uint64_t)status.size;
    }

    return true;
}

} // namespace

/**
 * No folder, no write.
 */
DataFiles::DataFiles()
    : _folder(nullptr), _writeFile(nullptr), _writeTarget(nullptr), _writeTemporary(nullptr), _writeSize(0),
      _writeReceived(0) {
}

/**
 * Drops an unfinished write, frees the folder.
 */
DataFiles::~DataFiles() {
    abortWrite();
    memory::release(_folder);
}

/**
 * Keeps a copy of the folder.
 */
void DataFiles::setFolder(const char *folder) {
    memory::release(_folder);
    _folder = nullptr;
    if (folder != nullptr) {
        _folder = memory::copyText(folder, strlen(folder));
    }
}

/**
 * Builds "<folder>/<relative>" checking each component of 'relative':
 * empty ones (a leading or doubled '/') and "." are skipped, ".." refused,
 * as a '\' or a ':' (Windows would read them as separators or drives).
 */
int DataFiles::resolve(const char *relative, size_t length, bool allowRoot, char **out) {
    // no data folder: no file system
    if (_folder == nullptr) {
        return ENOSYS;
    }

    // the folder, then each component
    char path[MAX_PATH];
    size_t used = (size_t)snprintf(path, sizeof(path), "%s", _folder);
    size_t start = 0;
    bool anyName = false;

    while (start <= length) {
        // the next component: up to '/' or the end
        size_t end = start;

        while (end < length && relative[end] != '/') {
            end++;
        }

        size_t size = end - start;
        const char *name = relative + start;
        start = end + 1;

        // skipped, refused, or added
        bool skipped = size == 0 || (size == 1 && name[0] == '.');
        if (skipped) {
            continue;
        }

        bool parent = size == 2 && name[0] == '.' && name[1] == '.';
        bool hasBackslash = memchr(name, '\\', size) != nullptr;
        bool hasColon = memchr(name, ':', size) != nullptr;
        bool hasNull = memchr(name, 0, size) != nullptr;
        bool foreign = hasBackslash || hasColon || hasNull;
        if (parent || foreign) {
            return EINVAL;
        }

        // room for it, and for the suffix of a temporary file after it
        if (used + 1 + size + sizeof(TEMPORARY_SUFFIX) >= sizeof(path)) {
            return ENAMETOOLONG;
        }

        path[used++] = '/';
        memcpy(path + used, name, size);
        used += size;
        path[used] = 0;
        anyName = true;
    }

    // the folder itself only where it makes sense (a listing)
    if (!anyName && !allowRoot) {
        return EINVAL;
    }

    *out = memory::copyText(path, used);
    return 0;
}

/**
 * The entries of a folder.
 */
int DataFiles::list(const char *path, size_t length, FileEntry **entries, int *count) {
    // the absolute path (the data folder itself allowed)
    char *folder = nullptr;
    int error = resolve(path, length, true, &folder);
    if (error != 0) {
        return error;
    }

    // a folder only
    files::Status status = {};
    if (!files::stat(folder, &status)) {
        error = errno;
    } else if (!status.isDirectory) {
        error = ENOTDIR;
    }

    // its entries
    Listing listing = { folder, nullptr, 0, 0 };
    if (error == 0 && !files::listDirectory(folder, &addEntry, &listing)) {
        error = errno;
    }

    // on failure, what was gathered is freed; else it goes to the caller
    memory::release(folder);
    if (error != 0) {
        freeEntries(listing.entries, listing.count);
        return error;
    }

    *entries = listing.entries;
    *count = listing.count;
    return 0;
}

/**
 * Frees the names, then the array.
 */
void DataFiles::freeEntries(FileEntry *entries, int count) {
    for (int index = 0; index < count; index++) {
        memory::release(entries[index].name);
    }

    memory::release(entries);
}

/**
 * Opens the file, reads a piece of it, closes it (no file stays open
 * between two requests).
 */
int DataFiles::read(const char *path, size_t length, int64_t offset, uint8_t *buffer, size_t capacity, size_t *got,
                    int64_t *fileSize) {
    // the absolute path
    char *file = nullptr;
    int error = resolve(path, length, false, &file);
    if (error != 0) {
        return error;
    }

    // a file, its size
    files::Status status = {};
    if (!files::stat(file, &status)) {
        error = errno;
    } else if (status.isDirectory) {
        error = EISDIR;
    } else if (offset < 0 || offset > status.size) {
        error = EINVAL;
    }

    // the piece asked
    FILE *opened = error == 0 ? files::open(file, "rb") : nullptr;
    if (error == 0 && opened == nullptr) {
        error = errno;
    }

    // read from 'offset', then closed at once
    if (opened != nullptr) {
        bool positioned = fseek(opened, (long)offset, SEEK_SET) == 0;
        *got = positioned ? fread(buffer, 1, capacity, opened) : 0;
        if (!positioned || ferror(opened)) {
            error = EIO;
        }

        fclose(opened);
        *fileSize = status.size;
    }

    memory::release(file);
    return error;
}

/**
 * Creates the temporary file next to the target.
 */
int DataFiles::beginWrite(const char *path, size_t length, int64_t size) {
    // an unfinished write is dropped; a negative size refused
    abortWrite();
    if (size < 0) {
        return EINVAL;
    }

    // the absolute path of the target
    char *target = nullptr;
    int error = resolve(path, length, false, &target);
    if (error != 0) {
        return error;
    }

    // never over a folder
    files::Status status = {};
    if (files::stat(target, &status) && status.isDirectory) {
        memory::release(target);
        return EISDIR;
    }

    // the temporary file (its parent must exist)
    size_t targetLength = strlen(target);
    char *temporary = (char *)memory::allocate(targetLength + sizeof(TEMPORARY_SUFFIX));
    memcpy(temporary, target, targetLength);
    memcpy(temporary + targetLength, TEMPORARY_SUFFIX, sizeof(TEMPORARY_SUFFIX));
    FILE *opened = files::open(temporary, "wb");
    if (opened == nullptr) {
        error = errno;
        memory::release(temporary);
        memory::release(target);
        return error;
    }

    // the write in progress
    _writeFile = opened;
    _writeTarget = target;
    _writeTemporary = temporary;
    _writeSize = size;
    _writeReceived = 0;
    return 0;
}

/**
 * Appends the bytes, in order, within the announced size.
 */
int DataFiles::writeChunk(int64_t offset, const uint8_t *data, size_t size) {
    // a write in progress, the next bytes in order, within the announced size
    bool inOrder = _writeFile != nullptr && offset == _writeReceived;
    bool fits = _writeReceived + (int64_t)size <= _writeSize;
    if (!inOrder || !fits) {
        return EINVAL;
    }

    // appended; a failed write drops the whole file
    if (fwrite(data, 1, size, _writeFile) != size) {
        int error = errno != 0 ? errno : EIO;
        abortWrite();
        return error;
    }

    _writeReceived += (int64_t)size;
    return 0;
}

/**
 * Closes the temporary file (the file system commits it then), renames it
 * onto the target.
 */
int DataFiles::endWrite() {
    // a write in progress, every byte received
    if (_writeFile == nullptr || _writeReceived != _writeSize) {
        return EINVAL;
    }

    // closed: its content is complete on the storage
    int error = 0;
    if (fclose(_writeFile) != 0) {
        error = errno != 0 ? errno : EIO;
    }

    _writeFile = nullptr;

    // in place of the target
    if (error == 0 && !files::rename(_writeTemporary, _writeTarget)) {
        error = errno;
    }

    // a failure leaves no temporary file behind
    if (error != 0) {
        files::removeFile(_writeTemporary);
    }

    // no write in progress any more
    memory::release(_writeTemporary);
    memory::release(_writeTarget);
    _writeTemporary = nullptr;
    _writeTarget = nullptr;
    return error;
}

/**
 * Closes and removes the temporary file.
 */
void DataFiles::abortWrite() {
    // the temporary file, if a write is in progress
    if (_writeFile != nullptr) {
        fclose(_writeFile);
        files::removeFile(_writeTemporary);
    }

    // its paths
    memory::release(_writeTemporary);
    memory::release(_writeTarget);
    _writeFile = nullptr;
    _writeTemporary = nullptr;
    _writeTarget = nullptr;
}

/**
 * A file, or an empty folder.
 */
int DataFiles::remove(const char *path, size_t length) {
    // the absolute path (never the data folder itself)
    char *target = nullptr;
    int error = resolve(path, length, false, &target);
    if (error != 0) {
        return error;
    }

    // removed as a folder or as a file, by its type
    files::Status status = {};
    bool removed = false;
    if (files::stat(target, &status)) {
        if (status.isDirectory) {
            removed = files::removeDirectory(target);
        } else {
            removed = files::removeFile(target);
        }
    }

    error = removed ? 0 : errno;
    memory::release(target);
    return error;
}

/**
 * One folder.
 */
int DataFiles::makeDirectory(const char *path, size_t length) {
    char *target = nullptr;
    int error = resolve(path, length, false, &target);
    if (error != 0) {
        return error;
    }

    error = files::makeDirectory(target) ? 0 : errno;
    memory::release(target);
    return error;
}

/**
 * Checks the types of both ends, then renames.
 */
int DataFiles::rename(const char *from, size_t fromLength, const char *to, size_t toLength) {
    // the absolute paths of both ends
    char *source = nullptr;
    char *target = nullptr;
    int error = resolve(from, fromLength, false, &source);
    if (error == 0) {
        error = resolve(to, toLength, false, &target);
    }

    // the source must exist; a folder never replaces anything, a file never
    // replaces a folder
    files::Status sourceStatus = {};
    files::Status targetStatus = {};
    if (error == 0 && !files::stat(source, &sourceStatus)) {
        error = errno;
    }

    bool targetExists = error == 0 && files::stat(target, &targetStatus);
    if (error == 0 && targetExists && sourceStatus.isDirectory) {
        error = EEXIST;
    } else if (error == 0 && targetExists && targetStatus.isDirectory) {
        error = EISDIR;
    }

    // rename
    if (error == 0 && !files::rename(source, target)) {
        error = errno;
    }

    memory::release(source);
    memory::release(target);
    return error;
}

/**
 * The sizes of every file, folders walked.
 */
int DataFiles::usedBytes(uint64_t *used) {
    // no data folder: no file system
    if (_folder == nullptr) {
        return ENOSYS;
    }

    *used = 0;
    addFolderUsage(_folder, used);
    return 0;
}

} // namespace agent
