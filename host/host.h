/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The host tool runtime: the common program runner (program.h) plus the
// source files on disk as modules.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rt {
class Permissions;
}

namespace host {

// How a program of the PC is launched (permissions_host.h): what its
// command line grants.
struct Launch {
    rt::Permissions *permissions;  // the options of qjsx run (nothing granted without them); completed
                                   // from the package's manifest; borrowed, lives during the run
    const char      *rootFolder;   // --qjsx-root-folder=<folder>; nullptr: none (borrowed)
};

/**
 * Runs a script (ES module) or a package (.qjspkg, from bytecode like a target),
 * then the event loop, with the permissions of 'launch' (a package adds the
 * "desktop" section of its manifest; ${executableFolder} is the package's
 * folder). Returns the exit code. process.argv = [executable, path, arguments...]
 */
int runFile(const char *path, int argumentCount, char **arguments, const Launch *launch);

/**
 * Runs a package already in memory ('data', 'size' bytes, kept valid during
 * the call), from its bytecode, with the permissions of 'launch' plus the
 * "desktop" section of its manifest ('programFolder' is ${executableFolder}).
 * Returns the exit code (1 if the permissions are wrong: nothing runs).
 * process.argv = 'arguments' as given.
 */
int runPackageData(const uint8_t *data, size_t size, const char *programFolder, int argumentCount, char **arguments,
                   const Launch *launch);

/**
 * Runs the command-line tool: main(process.argv.slice(1)) of the embedded module
 * "qjsx:cli". Returns the exit code. process.argv = [executable, arguments...]
 */
int runCommand(int argumentCount, char **arguments);

/**
 * Full path of the running executable ('/' separators), in a string
 * allocated with memory::allocate that the caller owns; nullptr if the
 * system cannot tell. Implemented in host/platform/executable_path.cpp.
 */
char *executablePath();

} // namespace host
