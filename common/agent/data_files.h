/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The files of the device's data folder, reached by the host through the
// agent (FILE_* requests: qjsx device ls | read | write | rm | mkdir | mv).
//
// Paths are relative to the data folder, '/' separators; a leading '/' is
// the data folder itself; "." components are skipped; ".." and empty names
// are refused (EINVAL): nothing outside the folder can be reached. A file is
// written to a temporary file next to it ("<name>.qjsx-part"), renamed onto
// it once complete: a cut transfer never leaves half a file, and a program
// reading it sees the old content or the new one. Every method returns 0 or
// an errno (ENOSYS: the device has no file system). Transport thread only.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace agent {

// One entry of a folder.
struct FileEntry {
    char   *name;          // malloc'd
    int64_t size;          // bytes (0 for a folder)
    bool    isDirectory;   // a folder
};

class DataFiles {
public:
    /**
     * No folder (setFolder() gives it), no write in progress.
     */
    DataFiles();

    /**
     * Drops an unfinished write (its temporary file removed).
     */
    ~DataFiles();

    DataFiles(const DataFiles &) = delete;
    DataFiles &operator=(const DataFiles &) = delete;

    /**
     * The data folder (copied), or nullptr: no file system.
     */
    void        setFolder(const char *folder);

    /**
     * The entries of folder 'path' (malloc'd array in *entries, freed by
     * freeEntries), temporary files of an unfinished write left out.
     */
    int         list(const char *path, size_t length, FileEntry **entries, int *count);

    /**
     * Frees what list() gave.
     */
    static void freeEntries(FileEntry *entries, int count);

    /**
     * Reads at most 'capacity' bytes of file 'path' from 'offset' into
     * 'buffer'; *got: bytes read, *fileSize: size of the whole file.
     */
    int          read(const char *path, size_t length, int64_t offset, uint8_t *buffer, size_t capacity, size_t *got,
                      int64_t *fileSize);

    /**
     * Starts writing file 'path' ('size' bytes coming); an unfinished write
     * is dropped. The parent folder must exist; 'path' must not be a folder.
     */
    int         beginWrite(const char *path, size_t length, int64_t size);

    /**
     * The next bytes of the write in progress, at 'offset' (in order).
     */
    int         writeChunk(int64_t offset, const uint8_t *data, size_t size);

    /**
     * Every byte arrived: the file replaces 'path'.
     */
    int         endWrite();

    /**
     * Drops the write in progress, if any (its temporary file removed).
     */
    void        abortWrite();

    /**
     * Removes file 'path', or folder 'path' if empty (ENOTEMPTY otherwise).
     * The data folder itself cannot be removed (EINVAL).
     */
    int         remove(const char *path, size_t length);

    /**
     * Creates folder 'path' (its parent must exist; EEXIST if it does).
     */
    int         makeDirectory(const char *path, size_t length);

    /**
     * Renames 'from' to 'to': a file replaces a file (as Node's renameSync);
     * a folder never replaces anything, a file never replaces a folder.
     */
    int         rename(const char *from, size_t fromLength, const char *to, size_t toLength);

    /**
     * Bytes used by the files of the data folder (all its folders), in *used.
     */
    int         usedBytes(uint64_t *used);

private:
    /**
     * The absolute path of 'relative' (length bytes) in a malloc'd *out.
     * 'allowRoot': the empty path, the folder itself, is accepted.
     */
    int         resolve(const char *relative, size_t length, bool allowRoot, char **out);

    char       *_folder;         // malloc'd; nullptr: no file system
    FILE       *_writeFile;      // the temporary file being written; nullptr: no write
    char       *_writeTarget;    // malloc'd: the path it will replace
    char       *_writeTemporary; // malloc'd: "<target>.qjsx-part"
    int64_t     _writeSize;      // bytes announced
    int64_t     _writeReceived;  // bytes written so far
};

} // namespace agent
