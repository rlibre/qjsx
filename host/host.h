/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The host tool runtime: the common program runner (program.h) plus the
// source files on disk as modules.
#pragma once

namespace host {

/**
 * Runs a script (ES module) or a package (.qjspkg, from bytecode like a target),
 * then the event loop. Returns the exit code. process.argv = [executable, path, arguments...]
 */
int runFile(const char *path, int argumentCount, char **arguments);

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
