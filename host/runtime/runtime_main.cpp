/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Entry point of qjsx-runtime: the runner of a program made into an
// executable of the PC by "qjsx build --executable" (host/js/executable.js).
// The engine has no compiler (no eval, as on a device); the native modules
// are those of the PC (sys/fs, sys/crypto, sys/net, device/serial).
//
// The program is a package appended at the end of a copy of this
// executable, followed by a trailer of TRAILER_SIZE bytes:
//
//   [qjsx-runtime] [package] ["QJSXPKG1"] [size of the package: 64 bits, little endian]
//
// Without a package appended, "qjsx-runtime <file.qjspkg> [arguments]" runs
// a package file (tests).
#include <stdio.h>
#include <string.h>

#include "files.h"
#include "host.h"
#include "memory.h"
#include "modules.h"
#include "device_modules.h"
#include "serial_host.h"
#include "permissions.h"
#include "permissions_host.h"
#include "version.h"

/**
 * Creates the native module "sys/net" of the host (host/modules/net.cpp).
 */
JSModuleDef *jsNewModule_net(JSContext *ctx, const char *name);

namespace {

const char TRAILER_MAGIC[] = "QJSXPKG1";   // 8 bytes, no terminating zero used
const size_t MAGIC_SIZE = 8;               // bytes of TRAILER_MAGIC
const size_t TRAILER_SIZE = 16;            // magic + 64-bit size

/**
 * Reads a 64-bit little endian value.
 */
uint64_t readLittleEndian64(const uint8_t *bytes) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) {
        value = (value << 8) | bytes[i];
    }

    return value;
}

/**
 * The executable file 'path' read whole into *file (*fileSize bytes, owned
 * by the caller); the package appended at its end in *package (inside
 * *file), *packageSize bytes. False (and *file nullptr) if there is none.
 */
bool readAppendedPackage(const char *path, uint8_t **file, size_t *fileSize, const uint8_t **package, size_t *packageSize) {
    // the whole executable (a few MB)
    *file = files::readAll(path, fileSize);
    if (*file == nullptr || *fileSize < TRAILER_SIZE) {
        memory::release(*file);
        *file = nullptr;
        return false;
    }

    // the trailer at its very end: the magic, then the size
    const uint8_t *trailer = *file + *fileSize - TRAILER_SIZE;
    uint64_t size = readLittleEndian64(trailer + MAGIC_SIZE);
    bool marked = memcmp(trailer, TRAILER_MAGIC, MAGIC_SIZE) == 0;
    bool fits = size > 0 && size <= *fileSize - TRAILER_SIZE;
    if (!marked || !fits) {
        memory::release(*file);
        *file = nullptr;
        return false;
    }

    // the package just before the trailer
    *package = trailer - size;
    *packageSize = (size_t)size;
    return true;
}

/**
 * True if 'text' ends with 'suffix'.
 */
bool endsWith(const char *text, const char *suffix) {
    size_t textLength = strlen(text);
    size_t suffixLength = strlen(suffix);
    return textLength >= suffixLength && strcmp(text + textLength - suffixLength, suffix) == 0;
}

/**
 * Runs the package appended to this executable: process.argv = [the
 * executable, the executable, arguments...] (as a standalone Node
 * executable: argv.slice(2) are the arguments). Returns the exit code.
 */
int runAppended(const uint8_t *package, size_t size, int argc, char **argv) {
    // the command line: the executable twice, then the arguments (on the
    // heap: no array on the stack); --qjsx-root-folder=<folder> is the
    // runtime's, the program does not see it
    rt::Permissions permissions;
    host::Launch launch = { &permissions, nullptr };
    size_t rootLength = strlen(host::ROOT_FOLDER_OPTION);
    char **arguments = (char **)memory::allocate(sizeof(char *) * (size_t)(argc + 1));
    int count = 0;
    arguments[count++] = argv[0];
    arguments[count++] = argv[0];

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], host::ROOT_FOLDER_OPTION, rootLength) == 0) {
            launch.rootFolder = argv[i] + rootLength;
        } else {
            arguments[count++] = argv[i];
        }
    }

    // the permissions are those of the package (its "desktop" section),
    // ${executableFolder} the folder of this executable
    char *folder = host::folderOf(argv[0]);
    if (folder == nullptr) {
        fprintf(stderr, "qjsx-runtime: cannot find the folder of %s\n", argv[0]);
        memory::release(arguments);
        return 1;
    }

    // run; process.argv was copied into JS strings at the start
    int exitCode = host::runPackageData(package, size, folder, count, arguments, &launch);
    memory::release(folder);
    memory::release(arguments);
    return exitCode;
}

} // namespace

/**
 * 1. the full path of this executable ('/' separators);
 * 2. the native modules of the PC;
 * 3. the package appended to it, if any: run it;
 * 4. else a package file given on the command line;
 * 5. else say what this executable is.
 */
int main(int argc, char **argv) {
    // the path of the executable (read to find the package, and argv[0])
    char *executable = host::executablePath();
    if (executable != nullptr) {
        argv[0] = executable;
    }

    files::useSlashes(argv[0]);

    // the native modules, before any runtime exists
    rt::registerNativeModule("sys/net", &jsNewModule_net);
    setSerialDriver(host::serialDriver());
    rt::registerNativeModule("device/serial", &jsNewModule_serial);

    // the package appended to this executable
    uint8_t *file = nullptr;
    size_t fileSize = 0;
    const uint8_t *package = nullptr;
    size_t packageSize = 0;
    if (readAppendedPackage(argv[0], &file, &fileSize, &package, &packageSize)) {
        int exitCode = runAppended(package, packageSize, argc, argv);
        memory::release(file);
        return exitCode;
    }

    // a package file on the command line, with its own permissions
    if (argc >= 2 && endsWith(argv[1], ".qjspkg")) {
        rt::Permissions permissions;
        host::Launch launch = { &permissions, nullptr };
        return host::runFile(argv[1], argc, argv, &launch);
    }

    // nothing to run
    fprintf(stderr, "qjsx-runtime %s: no program in this executable (made by 'qjsx build --executable'),\n"
                    "or run a package: qjsx-runtime <file.qjspkg> [arguments]\n", QJSX_VERSION);
    return 1;
}
