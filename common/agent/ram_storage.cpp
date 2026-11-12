/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The package loaded in RAM (see ram_storage.h).
#include "ram_storage.h"
#include "memory.h"

#include <stdio.h>
#include <string.h>

namespace agent {

// A package in RAM, with its references: the current slot holds one, the
// runner one while it runs it. Freed with the last one.
struct RamPackage {
    uint8_t *data;          // the package bytes (memory::tryAllocate), owned
    size_t   size;          // bytes
    char     buildId[64];   // null terminated
    int      references;    // under RamStorage::_mutex
};

namespace {

/**
 * A new package of 'size' bytes, one reference; nullptr if the RAM cannot
 * hold its bytes.
 */
RamPackage *newPackage(size_t size) {
    uint8_t *data = (uint8_t *)memory::tryAllocate(size);
    if (data == nullptr) {
        return nullptr;
    }

    RamPackage *package = (RamPackage *)memory::allocate(sizeof(RamPackage));
    *package = RamPackage{ data, size, "", 1 };
    return package;
}

/**
 * Frees a package and its bytes.
 */
void freePackage(RamPackage *package) {
    memory::release(package->data);
    memory::release(package);
}

} // namespace

/**
 * Frees what is left.
 */
RamStorage::~RamStorage() {
    clear();
    abortUpload();
}

/**
 * Allocates the coming package; a previous upload left unfinished is
 * dropped.
 */
bool RamStorage::beginUpload(size_t size) {
    // the room, exactly the size of the package
    RamPackage *package = newPackage(size);
    if (package == nullptr) {
        fprintf(stderr, "qjsx agent: not enough RAM for a package of %u bytes\n", (unsigned)size);
        return false;
    }

    // it replaces an upload left unfinished
    _mutex.lock();
    RamPackage *previous = _upload;
    _upload = package;
    _mutex.unlock();

    if (previous != nullptr) {
        freePackage(previous);
    }

    return true;
}

/**
 * Copies a chunk (the agent checks the order and the size).
 */
bool RamStorage::writeUpload(size_t offset, const uint8_t *data, size_t size) {
    _mutex.lock();
    bool fits = _upload != nullptr && offset + size <= _upload->size;
    if (fits) {
        memcpy(_upload->data + offset, data, size);
    }

    _mutex.unlock();
    return fits;
}

/**
 * The upload becomes the current package; the previous one loses its
 * reference.
 */
bool RamStorage::finishUpload(const char *buildId) {
    _mutex.lock();
    RamPackage *package = _upload;
    if (package == nullptr) {
        _mutex.unlock();
        return false;
    }

    // name it, make it the current one
    snprintf(package->buildId, sizeof(package->buildId), "%s", buildId);
    RamPackage *previous = _current;
    _current = package;
    _upload = nullptr;

    // the previous one: freed unless the runner still runs it
    if (previous != nullptr) {
        unreference(previous);
    }

    _mutex.unlock();
    return true;
}

/**
 * Frees the coming package.
 */
void RamStorage::abortUpload() {
    _mutex.lock();
    RamPackage *package = _upload;
    _upload = nullptr;
    _mutex.unlock();

    if (package != nullptr) {
        freePackage(package);
    }
}

/**
 * The current package's build id and size.
 */
bool RamStorage::installed(char *buildId, size_t buildIdSize, size_t *packageSize) {
    _mutex.lock();
    bool present = _current != nullptr;
    if (present) {
        snprintf(buildId, buildIdSize, "%s", _current->buildId);
        *packageSize = _current->size;
    }

    _mutex.unlock();
    return present;
}

/**
 * Takes a reference on the current package for the runner.
 */
const uint8_t *RamStorage::acquire(size_t *size, char *buildId, size_t buildIdSize) {
    _mutex.lock();
    RamPackage *package = _current;
    if (package == nullptr) {
        _mutex.unlock();
        return nullptr;
    }

    // one more reference, kept by the runner until release()
    package->references++;
    _running = package;
    *size = package->size;
    snprintf(buildId, buildIdSize, "%s", package->buildId);
    _mutex.unlock();
    return package->data;
}

/**
 * Drops the runner's reference.
 */
void RamStorage::release(const uint8_t *data) {
    _mutex.lock();
    RamPackage *package = _running;
    if (package != nullptr && package->data == data) {
        _running = nullptr;
        unreference(package);
    }

    _mutex.unlock();
}

/**
 * Drops the current package's reference.
 */
void RamStorage::clear() {
    _mutex.lock();
    RamPackage *package = _current;
    _current = nullptr;
    if (package != nullptr) {
        unreference(package);
    }

    _mutex.unlock();
}

/**
 * One reference less; the last one frees the package. _mutex held.
 */
void RamStorage::unreference(RamPackage *package) {
    package->references--;
    if (package->references == 0) {
        freePackage(package);
    }
}

} // namespace agent
