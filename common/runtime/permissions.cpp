/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What a program may reach of the machine (see permissions.h).
#include "permissions.h"
#include "files.h"
#include "memory.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace rt {

namespace {

thread_local const Permissions *currentPermissions = nullptr;   // of this thread's run

/**
 * Adds a copy of 'item' to the list.
 */
void addItem(PermissionList *list, const char *item) {
    size_t size = sizeof(char *) * (size_t)(list->count + 1);
    list->items = (char **)memory::reallocate(list->items, size);
    list->items[list->count++] = memory::copyText(item, strlen(item));
}

/**
 * Frees the strings of the list and the list.
 */
void freeList(PermissionList *list) {
    for (int i = 0; i < list->count; i++) {
        memory::release(list->items[i]);
    }

    memory::release(list->items);
    *list = PermissionList{ nullptr, 0, false };
}

/**
 * Compares the first 'length' bytes of two names: without case on Windows
 * (its file systems, ports and hosts ignore it), exactly elsewhere.
 */
bool sameText(const char *a, const char *b, size_t length) {
#if defined(_WIN32)
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }

    return true;
#else
    return strncmp(a, b, length) == 0;
#endif
}

/**
 * True if the real path 'path' is 'folder' or lies under it.
 */
bool isUnder(const char *path, const char *folder) {
    // the folder must be a prefix of the path
    size_t length = strlen(folder);
    if (strlen(path) < length || !sameText(path, folder, length)) {
        return false;
    }

    // ... ending at a separator: "C:/data" covers "C:/data/x", not "C:/database"
    bool folderEndsWithSlash = length > 0 && folder[length - 1] == '/';
    return folderEndsWithSlash || path[length] == 0 || path[length] == '/';
}

/**
 * Host names are compared without case (DNS ignores it).
 */
bool sameHost(const char *a, const char *b, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }

    return true;
}

/**
 * True if the granted 'entry' ("host" or "host:port") covers host:port. An
 * entry with several ':' and no brackets is an IPv6 address without port.
 */
bool netEntryCovers(const char *entry, const char *host, int port) {
    // split the entry: a ':' followed by digits only ends it with a port
    const char *colon = strrchr(entry, ':');
    bool isBareIpv6 = colon != nullptr && strchr(entry, ':') != colon && entry[0] != '[';
    bool hasPort = colon != nullptr && !isBareIpv6 && colon[1] != 0
                   && strspn(colon + 1, "0123456789") == strlen(colon + 1);
    size_t hostLength = hasPort ? (size_t)(colon - entry) : strlen(entry);

    // "[::1]:80": the host without its brackets
    const char *entryHost = entry;
    bool bracketed = hostLength >= 2 && entry[0] == '[' && entry[hostLength - 1] == ']';
    if (bracketed) {
        entryHost = entry + 1;
        hostLength -= 2;
    }

    // the same host, and the same port if the entry names one
    bool hostMatches = strlen(host) == hostLength && sameHost(host, entryHost, hostLength);
    bool portMatches = !hasPort || atoi(colon + 1) == port;
    return hostMatches && portMatches;
}

} // namespace

// ### Permissions ##############################################################

/**
 * Nothing allowed.
 */
Permissions::Permissions()
    : _workingFolder(nullptr), _fileSystem(true), _read{ nullptr, 0, false }, _write{ nullptr, 0, false },
      _net{ nullptr, 0, false }, _env{ nullptr, 0, false }, _serial{ nullptr, 0, false }, _hint(nullptr) {
}

/**
 * Frees the lists.
 */
Permissions::~Permissions() {
    memory::release(_workingFolder);
    memory::release(_hint);
    freeList(&_read);
    freeList(&_write);
    freeList(&_net);
    freeList(&_env);
    freeList(&_serial);
}

/**
 * The program's current folder.
 */
void Permissions::setWorkingFolder(const char *folder) {
    memory::release(_workingFolder);
    _workingFolder = folder != nullptr ? memory::copyText(folder, strlen(folder)) : nullptr;
}

/**
 * No file system.
 */
void Permissions::setNoFileSystem() {
    _fileSystem = false;
}

/**
 * Every kind, without limit.
 */
void Permissions::allowEverything() {
    _read.everything = true;
    _write.everything = true;
    _net.everything = true;
    _env.everything = true;
    _serial.everything = true;
}

/**
 * A folder, by its real path.
 */
bool Permissions::allowFiles(Access access, const char *folder) {
    // its real path now: what the checks compare with
    char *real = files::realPath(folder);
    if (real == nullptr) {
        return false;
    }

    addItem(access == Access::read ? &_read : &_write, real);
    memory::release(real);
    return true;
}

/**
 * Everywhere.
 */
void Permissions::allowFilesEverywhere(Access access) {
    PermissionList *list = access == Access::read ? &_read : &_write;
    list->everything = true;
}

/**
 * A host, or every host.
 */
void Permissions::allowNet(const char *hostPort) {
    if (hostPort == nullptr) {
        _net.everything = true;
        return;
    }

    addItem(&_net, hostPort);
}

/**
 * A variable, or every variable.
 */
void Permissions::allowEnv(const char *name) {
    if (name == nullptr) {
        _env.everything = true;
        return;
    }

    addItem(&_env, name);
}

/**
 * A port, or every port.
 */
void Permissions::allowSerial(const char *port) {
    if (port == nullptr) {
        _serial.everything = true;
        return;
    }

    addItem(&_serial, port);
}

/**
 * How to grant a refused access.
 */
void Permissions::setHint(const char *hint) {
    memory::release(_hint);
    _hint = hint != nullptr ? memory::copyText(hint, strlen(hint)) : nullptr;
}

/**
 * The program's current folder.
 */
const char *Permissions::workingFolder() const {
    return _workingFolder;
}

/**
 * A file system or not.
 */
bool Permissions::hasFileSystem() const {
    return _fileSystem;
}

/**
 * 1. granted everywhere: yes;
 * 2. the real path of 'path' (a dangling link or an unreadable path: no);
 * 3. under one of the granted folders.
 */
bool Permissions::mayAccess(const char *path, Access access) const {
    // everywhere
    const PermissionList *list = access == Access::read ? &_read : &_write;
    if (list->everything) {
        return true;
    }

    // the real path
    char *real = files::realPath(path);
    if (real == nullptr) {
        errno = EACCES;
        return false;
    }

    // under a granted folder
    bool allowed = false;
    for (int i = 0; i < list->count && !allowed; i++) {
        allowed = isUnder(real, list->items[i]);
    }

    memory::release(real);
    if (!allowed) {
        errno = EACCES;
    }

    return allowed;
}

/**
 * A granted host (and port).
 */
bool Permissions::mayUseNet(const char *host, int port) const {
    if (_net.everything) {
        return true;
    }

    for (int i = 0; i < _net.count; i++) {
        if (netEntryCovers(_net.items[i], host, port)) {
            return true;
        }
    }

    return false;
}

/**
 * A granted variable (names compared exactly, as on POSIX; Windows ignores
 * their case).
 */
bool Permissions::mayUseEnv(const char *name) const {
    if (_env.everything) {
        return true;
    }

    size_t length = strlen(name);
    for (int i = 0; i < _env.count; i++) {
        if (strlen(_env.items[i]) == length && sameText(_env.items[i], name, length)) {
            return true;
        }
    }

    return false;
}

/**
 * A granted port; nullptr: some port granted.
 */
bool Permissions::mayUseSerial(const char *port) const {
    if (_serial.everything) {
        return true;
    }

    if (port == nullptr) {
        return _serial.count > 0;
    }

    size_t length = strlen(port);
    for (int i = 0; i < _serial.count; i++) {
        if (strlen(_serial.items[i]) == length && sameText(_serial.items[i], port, length)) {
            return true;
        }
    }

    return false;
}

/**
 * The end of a refusal message.
 */
const char *Permissions::hint() const {
    return _hint != nullptr ? _hint : "";
}

// ### The thread's run #########################################################

/**
 * The permissions of this thread's run.
 */
void setPermissions(const Permissions *permissions) {
    currentPermissions = permissions;
}

/**
 * The permissions of this thread's run; nullptr: everything allowed.
 */
const Permissions *permissions() {
    return currentPermissions;
}

/**
 * "read" or "write".
 */
const char *accessName(Access access) {
    return access == Access::read ? "read" : "write";
}

} // namespace rt
