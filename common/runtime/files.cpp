/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

#include "files.h"
#include "memory.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <sys/stat.h>
#include <io.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#endif

namespace files {

#if defined(_WIN32)

namespace {

/**
 * Converts UTF-8 to UTF-16 in a malloc'd string (freed by the caller).
 * Returns nullptr with errno = EINVAL if the text is not valid.
 */
wchar_t *widen(const char *text) {
    // measure the UTF-16 length, terminating zero included; 0 means invalid UTF-8
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (count <= 0) {
        errno = EINVAL;
        return nullptr;
    }

    // allocate and convert
    wchar_t *wide = (wchar_t *)memory::allocate(count * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, count);
    return wide;
}

/**
 * Converts UTF-16 to UTF-8 in a malloc'd string (freed by the caller).
 * Never fails: an empty string is returned if the conversion is impossible.
 */
char *narrow(const wchar_t *wide) {
    // measure the UTF-8 length; an impossible conversion gives an empty string
    int count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    char *text = (char *)memory::allocate(count > 0 ? count : 1);
    if (count <= 0) {
        text[0] = 0;
        return text;
    }

    // convert
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, count, nullptr, nullptr);
    return text;
}

/**
 * Translates a Win32 error (GetLastError) into errno (unknown errors become
 * EIO), for the functions that call the Win32 API instead of the C runtime.
 */
void setErrnoFromWin32(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: {
            errno = ENOENT;
            break;
        }
        case ERROR_ACCESS_DENIED: {
            errno = EACCES;
            break;
        }
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS: {
            errno = EEXIST;
            break;
        }
        case ERROR_DIR_NOT_EMPTY: {
            errno = ENOTEMPTY;
            break;
        }
        case ERROR_SHARING_VIOLATION: {
            errno = EBUSY;
            break;
        }
        default: {
            errno = EIO;
            break;
        }
    }
}

/**
 * fopen with a UTF-8 path ('mode' is already wide, e.g. L"rb").
 */
FILE *openFile(const char *path, const wchar_t *mode) {
    // convert the path to UTF-16
    wchar_t *wide = widen(path);
    if (wide == nullptr) {
        return nullptr;
    }

    // open with the wide API, which sets errno on failure
    FILE *file = _wfopen(wide, mode);
    memory::release(wide);
    return file;
}

/**
 * Size in bytes of an open file, read by seeking to its end, and back to its
 * start; -1 with errno set on failure. 64-bit offsets: fseek / ftell stop at
 * 2 GB on Windows, where long has 32 bits.
 */
int64_t measureFile(FILE *file) {
    // to the end, read the position
    if (_fseeki64(file, 0, SEEK_END) != 0) {
        return -1;
    }

    int64_t length = _ftelli64(file);

    // back to the start for the reads
    if (_fseeki64(file, 0, SEEK_SET) != 0) {
        return -1;
    }

    return length;
}

/**
 * True if the path names an existing directory (a drive root like "D:" or
 * "D:/" included).
 */
bool isDirectoryPath(const char *path) {
    // convert the path to UTF-16
    wchar_t *wide = widen(path);
    if (wide == nullptr) {
        return false;
    }

    // read the attributes of the entry
    DWORD attributes = GetFileAttributesW(wide);
    memory::release(wide);
    bool exists = attributes != INVALID_FILE_ATTRIBUTES;
    return exists && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

} // namespace

/**
 * Windows version: fills 'status' with _wstat64.
 */
bool stat(const char *path, Status *status) {
    // "does not exist" until the system says otherwise
    *status = Status{ false, false, false, false, 0, 0 };

    // convert the path to UTF-16
    wchar_t *wide = widen(path);
    if (wide == nullptr) {
        return false;
    }

    // ask the C runtime, which sets errno on failure
    struct _stat64 info;
    int result = _wstat64(wide, &info);
    memory::release(wide);
    if (result != 0) {
        return false;
    }

    // only the regular file and directory bits are reported; the time has a
    // resolution of one second
    status->exists = true;
    status->isFile = (info.st_mode & _S_IFREG) != 0;
    status->isDirectory = (info.st_mode & _S_IFDIR) != 0;
    status->size = info.st_size;
    status->modifiedMs = (double)info.st_mtime * 1000.0;
    return true;
}

/**
 * Windows version: stat() of the path, then the attributes of the entry
 * itself: a reparse point is a symbolic link or a junction.
 */
bool linkStatus(const char *path, Status *status) {
    // a link to a missing target still exists: start from "nothing" and read
    // the attributes of the entry itself
    stat(path, status);

    wchar_t *wide = widen(path);
    if (wide == nullptr) {
        return false;
    }

    DWORD attributes = GetFileAttributesW(wide);
    memory::release(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        setErrnoFromWin32(GetLastError());
        *status = Status{ false, false, false, false, 0, 0 };
        return false;
    }

    // the entry describes itself: a link is neither a file nor a directory
    // to remove recursively; isDirectory says whether it is a directory link
    status->exists = true;
    status->isLink = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    status->isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    status->isFile = !status->isDirectory && !status->isLink;
    return true;
}

/**
 * Windows version: creates one directory with _wmkdir.
 */
bool makeDirectory(const char *path) {
    // convert the path to UTF-16
    wchar_t *wide = widen(path);
    if (wide == nullptr) {
        return false;
    }

    // create it; the C runtime sets errno on failure
    int result = _wmkdir(wide);
    memory::release(wide);
    return result == 0;
}

/**
 * Windows version: deletes a file with _wunlink.
 */
bool removeFile(const char *path) {
    // convert the path to UTF-16
    wchar_t *wide = widen(path);
    if (wide == nullptr) {
        return false;
    }

    // delete it; the C runtime sets errno on failure
    int result = _wunlink(wide);
    memory::release(wide);
    return result == 0;
}

/**
 * Windows version: deletes an empty directory with _wrmdir.
 */
bool removeDirectory(const char *path) {
    // convert the path to UTF-16
    wchar_t *wide = widen(path);
    if (wide == nullptr) {
        return false;
    }

    // delete it; the C runtime sets errno on failure
    int result = _wrmdir(wide);
    memory::release(wide);
    return result == 0;
}

/**
 * Windows version: renames or moves with MoveFileExW.
 */
bool rename(const char *from, const char *to) {
    // convert both paths; a failed conversion has set errno (EINVAL)
    wchar_t *wideFrom = widen(from);
    wchar_t *wideTo = widen(to);
    bool converted = wideFrom != nullptr && wideTo != nullptr;

    // move; unlike the C runtime's rename, MoveFileExW can replace an
    // existing 'to' and move to another volume (copy then delete)
    bool ok = false;
    if (converted) {
        ok = MoveFileExW(wideFrom, wideTo, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
        if (!ok) {
            setErrnoFromWin32(GetLastError());
        }
    }

    // free both paths whatever happened (one of them may be nullptr)
    memory::release(wideFrom);
    memory::release(wideTo);
    return ok;
}

/**
 * Windows version: lists the entries with FindFirstFileW / FindNextFileW.
 */
bool listDirectory(const char *path, DirectoryVisitor *visit, void *opaque) {
    // build the search pattern: the path, then '/' and '*' (2 more characters
    // and the zero)
    size_t length = strlen(path);
    char *pattern = (char *)memory::allocate(length + 3);
    snprintf(pattern, length + 3, "%s/*", path);

    // convert it to UTF-16
    wchar_t *wide = widen(pattern);
    memory::release(pattern);
    if (wide == nullptr) {
        return false;
    }

    // open the search: it already returns the first entry
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(wide, &entry);
    memory::release(wide);

    // a missing or unreadable directory fails here; but a drive root has no
    // "." nor "..": when it is empty, nothing matches (ERROR_FILE_NOT_FOUND)
    // in a directory that exists, which is an empty listing
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        bool isEmptyDirectory = error == ERROR_FILE_NOT_FOUND && isDirectoryPath(path);
        if (isEmptyDirectory) {
            return true;
        }

        setErrnoFromWin32(error);
        return false;
    }

    // visit each entry until visit returns false or there are no more
    bool ok = true;

    do {
        // "." and ".." are not given to visit
        bool isDot = wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0;
        if (isDot) {
            continue;
        }

        // give visit the name in UTF-8
        char *name = narrow(entry.cFileName);
        ok = visit(opaque, name);
        memory::release(name);
    } while (ok && FindNextFileW(search, &entry));

    // FindNextFileW failed (visit did not stop the listing): the end of the
    // list, or a read error, which fails the listing
    if (ok) {
        DWORD error = GetLastError();
        bool readFailed = error != ERROR_NO_MORE_FILES;
        if (readFailed) {
            setErrnoFromWin32(error);
            ok = false;
        }
    }

    // close the search; ok is false if visit stopped the listing (errno left
    // as visit set it) or a read failed
    FindClose(search);
    return ok;
}

/**
 * Windows version: _wgetcwd in UTF-8, with '/' separators.
 */
char *currentDirectory() {
    // ask the C runtime (it allocates the wide string)
    wchar_t *wide = _wgetcwd(nullptr, 0);
    if (wide == nullptr) {
        return nullptr;
    }

    // convert it to UTF-8
    char *text = narrow(wide);
    memory::release(wide);

    // '/' separators, like every path the runtime gives to JS
    useSlashes(text);
    return text;
}

#else

namespace {

/**
 * fopen: POSIX paths are already UTF-8 bytes.
 */
FILE *openFile(const char *path, const char *mode) {
    return fopen(path, mode);
}

/**
 * Size in bytes of an open file, read by seeking to its end, and back to its
 * start; -1 with errno set on failure. fseeko / ftello take an off_t, wider
 * than long on 32-bit systems built with 64-bit file offsets.
 */
int64_t measureFile(FILE *file) {
    // to the end, read the position
    if (fseeko(file, 0, SEEK_END) != 0) {
        return -1;
    }

    int64_t length = (int64_t)ftello(file);

    // back to the start for the reads
    if (fseeko(file, 0, SEEK_SET) != 0) {
        return -1;
    }

    return length;
}

} // namespace

/**
 * POSIX version: fills 'status' with stat(2).
 */
bool stat(const char *path, Status *status) {
    // "does not exist" until the system says otherwise
    *status = Status{ false, false, false, false, 0, 0 };

    // ask the system, which sets errno on failure
    struct ::stat info;
    if (::stat(path, &info) != 0) {
        return false;
    }

    // the time has a resolution of one second
    status->exists = true;
    status->isFile = S_ISREG(info.st_mode);
    status->isDirectory = S_ISDIR(info.st_mode);
    status->size = info.st_size;
    status->modifiedMs = (double)info.st_mtime * 1000.0;
    return true;
}

/**
 * POSIX version: lstat(2), which describes a symbolic link itself. newlib
 * (ESP32, RP2350): stat(2), its file systems have no links (and no lstat).
 */
bool linkStatus(const char *path, Status *status) {
    // "does not exist" until the system says otherwise
    *status = Status{ false, false, false, false, 0, 0 };

    // ask the system, which sets errno on failure
    struct ::stat info;
#if defined(__NEWLIB__)
    int failed = ::stat(path, &info);
#else
    int failed = ::lstat(path, &info);
#endif
    if (failed != 0) {
        return false;
    }

    // the time has a resolution of one second
    status->exists = true;
    status->isFile = S_ISREG(info.st_mode);
    status->isDirectory = S_ISDIR(info.st_mode);
    status->isLink = S_ISLNK(info.st_mode);
    status->size = info.st_size;
    status->modifiedMs = (double)info.st_mtime * 1000.0;
    return true;
}

/**
 * POSIX version: mkdir(2) with mode 0777 (reduced by the umask).
 */
bool makeDirectory(const char *path) {
    return mkdir(path, 0777) == 0;
}

/**
 * POSIX version: unlink(2).
 */
bool removeFile(const char *path) {
    return unlink(path) == 0;
}

/**
 * POSIX version: rmdir(2).
 */
bool removeDirectory(const char *path) {
    return rmdir(path) == 0;
}

/**
 * POSIX version: rename(2), which replaces an existing target (same file
 * system only).
 */
bool rename(const char *from, const char *to) {
    return ::rename(from, to) == 0;
}

/**
 * POSIX version: lists the entries with opendir / readdir.
 */
bool listDirectory(const char *path, DirectoryVisitor *visit, void *opaque) {
    // open the directory; the system sets errno on failure
    DIR *directory = opendir(path);
    if (directory == nullptr) {
        return false;
    }

    // give each entry but "." and ".." to visit, until it returns false or
    // there are no more
    bool ok = true;

    while (ok) {
        // next entry: readdir returns nullptr at the end AND on a read error,
        // told apart by errno, which it only sets on an error
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == nullptr) {
            ok = errno == 0;
            break;
        }

        // give it to visit
        bool isDot = strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0;
        if (!isDot) {
            ok = visit(opaque, entry->d_name);
        }
    }

    // close the directory, keeping errno for the caller; ok is false if
    // visit stopped the listing or a read failed
    int error = errno;
    closedir(directory);
    errno = error;
    return ok;
}

/**
 * POSIX version: getcwd with a malloc'd buffer of the right size.
 */
char *currentDirectory() {
    return getcwd(nullptr, 0);
}

#endif

namespace {

/**
 * After a creation failed with EEXIST: true if what is at 'path' can be used
 * as a directory. False if something else (a file) is in the way, with errno
 * EEXIST for the last component of the path, ENOTDIR for a parent (Node's
 * codes). A path stat cannot describe ("C:") counts as a directory: the
 * system said it exists.
 */
bool usableAsDirectory(const char *path, bool isLast) {
    // a directory, or something stat cannot describe: usable
    Status status;
    bool found = stat(path, &status);
    if (!found || status.isDirectory) {
        return true;
    }

    // anything else is in the way
    errno = isLast ? EEXIST : ENOTDIR;
    return false;
}

} // namespace

/**
 * Creates the directory and its missing parents; succeeds if it already
 * exists, fails if a file is in the way.
 */
bool makeDirectories(const char *path) {
    // room for a copy of each prefix of the path
    size_t length = strlen(path);
    char *partial = (char *)memory::allocate(length + 1);
    bool ok = true;

    // for each prefix that ends at a separator ('/' or '\') or at the end of
    // the path, until a creation fails
    for (size_t end = 1; end <= length && ok; end++) {
        bool isLast = end == length;
        bool isBoundary = isLast || path[end] == '/' || path[end] == '\\';
        if (!isBoundary) {
            continue;
        }

        // copy the prefix read so far
        memcpy(partial, path, end);
        partial[end] = 0;

        // already a directory: nothing to create
        Status status;
        bool isDirectory = stat(partial, &status) && status.isDirectory;
        if (isDirectory) {
            continue;
        }

        // create it; on failure errno is set by makeDirectory. EEXIST is not
        // an error if a directory is there after all ("C:", or another process
        // created it in between), but it is when a file is in the way
        bool created = makeDirectory(partial);
        ok = created || (errno == EEXIST && usableAsDirectory(partial, isLast));
    }

    memory::release(partial);
    return ok;
}

/**
 * Reads the whole file in binary mode, plus a terminating zero byte.
 */
uint8_t *readAll(const char *path, size_t *size) {
    // open in binary mode
#if defined(_WIN32)
    FILE *file = openFile(path, L"rb");
#else
    FILE *file = openFile(path, "rb");
#endif
    if (file == nullptr) {
        return nullptr;
    }

    // get the size (64 bits) by seeking to the end and back
    int64_t length = measureFile(file);
    if (length < 0) {
        fclose(file);
        errno = EIO;
        return nullptr;
    }

    // the buffer holds the file and a zero byte: a file too large for the
    // address space (32-bit targets) is EFBIG
    bool fitsInMemory = (uint64_t)length < (uint64_t)SIZE_MAX;
    if (!fitsInMemory) {
        fclose(file);
        errno = EFBIG;
        return nullptr;
    }

    // read it into a buffer one byte longer; a short read is EIO
    size_t byteCount = (size_t)length;
    uint8_t *data = (uint8_t *)memory::allocate(byteCount + 1);
    size_t read = fread(data, 1, byteCount, file);
    fclose(file);
    if (read != byteCount) {
        memory::release(data);
        errno = EIO;
        return nullptr;
    }

    // add the zero byte, not counted in the size, so text can be used as a C string
    data[byteCount] = 0;
    *size = byteCount;
    return data;
}

/**
 * Creates or truncates the file and writes the data in binary mode.
 */
bool writeAll(const char *path, const void *data, size_t size) {
    // create or truncate, in binary mode
#if defined(_WIN32)
    FILE *file = openFile(path, L"wb");
#else
    FILE *file = openFile(path, "wb");
#endif
    if (file == nullptr) {
        return false;
    }

    // write and close: the close flushes the buffered data, so it can fail
    // too; errno is EIO when the C runtime did not set it (cleared first: a
    // value left by an earlier call would be taken for the reason)
    errno = 0;
    size_t written = fwrite(data, 1, size, file);
    bool ok = fclose(file) == 0 && written == size;
    if (!ok && errno == 0) {
        errno = EIO;
    }

    return ok;
}

/**
 * Windows: '\' into '/', except in a "\\?\" or "\\.\" path. Elsewhere: nothing.
 */
void useSlashes(char *path) {
#if defined(_WIN32)
    // a long path or a device path keeps its backslashes
    bool isLongPath = strncmp(path, "\\\\?\\", 4) == 0;
    bool isDevicePath = strncmp(path, "\\\\.\\", 4) == 0;
    if (isLongPath || isDevicePath) {
        return;
    }

    // replace every backslash
    for (char *p = path; *p != 0; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
#else
    (void)path;
#endif
}

/**
 * "/..." everywhere; on Windows also "\...", "\\server...", and a drive letter.
 */
bool isAbsolute(const char *path) {
    // a root separator
    bool fromRoot = path[0] == '/';
#if defined(_WIN32)
    fromRoot = fromRoot || path[0] == '\\';

    // a drive letter: "C:/x", or "C:x", relative to that drive's own directory
    bool isLetter = (path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z');
    bool hasDrive = isLetter && path[1] == ':';
    fromRoot = fromRoot || hasDrive;
#endif
    return fromRoot;
}

} // namespace files
