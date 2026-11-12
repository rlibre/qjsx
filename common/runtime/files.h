/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// File system primitives with UTF-8 paths (on Windows, converted to UTF-16
// and passed to the wide API, so that non-ASCII paths work).
// Every function returns false / nullptr on failure, with errno set.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace files {

/**
 * Reads a whole file into a malloc'd buffer (freed by the caller), followed
 * by a zero byte that is not counted in *size, so text can be used as a C
 * string. Returns nullptr on failure.
 */
uint8_t *readAll(const char *path, size_t *size);

/**
 * Creates or truncates the file and writes 'size' bytes of 'data' into it.
 */
bool writeAll(const char *path, const void *data, size_t size);

struct Status {
    bool    exists;       // false: nothing at the path (other fields 0)
    bool    isFile;       // regular file
    bool    isDirectory;  // directory (stat: symbolic links followed)
    bool    isLink;       // linkStatus only: a symbolic link or a junction (Windows)
    int64_t size;         // bytes
    double  modifiedMs;   // milliseconds since 1970
};

/**
 * Fills 'status' for the path. Returns false, with status.exists false and
 * errno set, if the path does not exist or cannot be read.
 */
bool stat(const char *path, Status *status);

/**
 * Like stat(), but a symbolic link (or a Windows junction) is described
 * itself, not its target (lstat): isLink is set. On Windows isDirectory then
 * says whether the link is a directory link.
 */
bool linkStatus(const char *path, Status *status);

/**
 * Creates one directory (its parent must exist).
 */
bool makeDirectory(const char *path);

/**
 * Creates a directory and the missing parents too; succeeds if it already
 * exists.
 */
bool makeDirectories(const char *path);

/**
 * Deletes a file.
 */
bool removeFile(const char *path);

/**
 * Deletes a directory, which must be empty.
 */
bool removeDirectory(const char *path);

/**
 * Renames or moves a file or directory; replaces 'to' if it exists.
 */
bool rename(const char *from, const char *to);

// Calls 'visit' for each entry of the directory (not "." and "..").
// Stops and returns false if 'visit' returns false.
typedef bool DirectoryVisitor(void *opaque, const char *name);

/**
 * Calls 'visit' with the name (UTF-8, not a full path) of each entry of the
 * directory, "." and ".." excluded, in no particular order. Stops and returns
 * false as soon as 'visit' returns false (errno is then left as it was).
 */
bool listDirectory(const char *path, DirectoryVisitor *visit, void *opaque);

/**
 * Current directory (malloc'd string, '/' separators).
 */
char *currentDirectory();

/**
 * Windows: turns the '\' of 'path' into '/' in place (every Windows API accepts
 * '/'). A path starting with "\\?\" (long path) or "\\.\" (device, COM10...)
 * is left as it is: Windows reads it without normalizing, so it needs its '\'.
 * Elsewhere: nothing ('\' is an ordinary character of a POSIX name).
 */
void useSlashes(char *path);

/**
 * True if 'path' does not depend on a current directory: "/..." everywhere;
 * on Windows also "\...", "C:..." and "\\server..." (a drive-relative "C:x"
 * counts as absolute: it cannot be joined to another directory).
 */
bool isAbsolute(const char *path);


/**
 * The real path of 'path' (malloc'd, freed by the caller; absolute, '/'
 * separators): a relative path is taken from the current directory, "." and
 * ".." are resolved, and the links and junctions of the part that exists are
 * followed; the part that does not exist yet (a file about to be created) is
 * appended as written. nullptr with errno set if the path cannot be read, or
 * EACCES if its first missing component is a link to a missing target (a
 * write through it would land elsewhere). newlib (devices: no links): the
 * path normalized.
 */
char *realPath(const char *path);

/**
 * The value of an environment variable (malloc'd UTF-8, freed by the caller),
 * or nullptr if it is not set.
 */
char *environmentValue(const char *name);

} // namespace files
