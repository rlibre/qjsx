/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Full path of the running executable, for the desktop tool: it finds the
// files delivered next to it (stdlib.qjslib) from there.
#include "host.h"

#include "files.h"
#include "memory.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <stdint.h>
#else
#include <unistd.h>
#endif

namespace host {

#if defined(_WIN32)

/**
 * Windows: the path of the module of the process, converted from UTF-16 to
 * UTF-8, with '/' separators.
 */
char *executablePath() {
    // ask for the path in a buffer grown until it fits: a returned length
    // equal to the capacity means it was cut
    DWORD capacity = 260;
    wchar_t *wide = (wchar_t *)memory::allocate(capacity * sizeof(wchar_t));
    DWORD length = GetModuleFileNameW(nullptr, wide, capacity);

    while (length >= capacity) {
        capacity *= 2;
        wide = (wchar_t *)memory::reallocate(wide, capacity * sizeof(wchar_t));
        length = GetModuleFileNameW(nullptr, wide, capacity);
    }

    // the system cannot tell
    if (length == 0) {
        memory::release(wide);
        return nullptr;
    }

    // measure the UTF-8 text, terminating zero included, then convert
    int count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        memory::release(wide);
        return nullptr;
    }

    char *path = (char *)memory::allocate((size_t)count);
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, count, nullptr, nullptr);
    memory::release(wide);

    // '/' separators, like every path given to JS
    files::useSlashes(path);
    return path;
}

#elif defined(__APPLE__)

/**
 * macOS: the path the loader recorded for the executable (it may go through
 * symbolic links, which does not matter to find its folder).
 */
char *executablePath() {
    // a first call with no room gives the size needed, zero included
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);

    // the second call fills the buffer
    char *path = (char *)memory::allocate(size);
    if (_NSGetExecutablePath(path, &size) != 0) {
        memory::release(path);
        return nullptr;
    }

    return path;
}

#else

/**
 * Linux: the target of the link /proc/self/exe.
 */
char *executablePath() {
    // read the link in a buffer grown until the target fits with its
    // terminating zero (readlink does not write one, and cuts silently)
    size_t capacity = 256;
    char *path = (char *)memory::allocate(capacity);
    ssize_t length = readlink("/proc/self/exe", path, capacity);

    while (length >= 0 && (size_t)length >= capacity) {
        capacity *= 2;
        path = (char *)memory::reallocate(path, capacity);
        length = readlink("/proc/self/exe", path, capacity);
    }

    // the system cannot tell (no /proc)
    if (length < 0) {
        memory::release(path);
        return nullptr;
    }

    path[length] = 0;
    return path;
}

#endif

} // namespace host
