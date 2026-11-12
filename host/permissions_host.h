/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The permissions of a program of the PC (doc/architecture.md 6.7), on
// Deno's model: nothing is allowed unless granted.
//   - "qjsx run script.js": only what its options grant (readRunOption);
//   - a package ("qjsx run app.qjspkg", an executable made by "qjsx build
//     --executable"): the "desktop" section of its manifest, copied from
//     qjsx.json by qjsx build (applyDesktopSection), plus the options of
//     qjsx run if any:
//
//       "desktop": {
//           "rootFolder": "${userAppDataFolder}",
//           "permissions": { "read": ["${rootFolder}", "${executableFolder}/config"],
//                            "write": ["${rootFolder}"], "net": ["api.example.com:443"],
//                            "env": ["HOME"], "serial": ["COM3"] }
//       }
//
//     each kind is a list or true (everything); a path starts with a
//     variable or is absolute. A root folder without "permissions" may be
//     read, not written.
//
// The root folder is the program's process.cwd() (relative fs paths resolve
// there); it comes from --qjsx-root-folder=<folder>, else the environment
// variable QJSX_ROOT_FOLDER, else "rootFolder" of the manifest. A root at
// the root of a file system ("/", "C:/") is refused.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "permissions.h"

namespace host {

// The root folder on a command line ("qjsx run", an executable): the only
// option an executable takes for itself (the program does not see it).
const char *const ROOT_FOLDER_OPTION = "--qjsx-root-folder=";

/**
 * Reads one option of "qjsx run" (given before the script) into
 * *permissions, or the root folder into *rootFolder (borrowed from 'word'):
 *   -A, --allow-all                      everything
 *   -R, --allow-read[=folder,...]        read (everywhere without a list)
 *   -W, --allow-write[=folder,...]       write
 *   -N, --allow-net[=host[:port],...]    network
 *   -E, --allow-env[=NAME,...]           environment variables
 *   --allow-serial[=port,...]            serial ports
 *   --qjsx-root-folder=<folder>          root folder (process.cwd())
 * Returns false if 'word' is not one of them, or after printing why its
 * value is wrong (*known tells which).
 */
bool readRunOption(const char *word, rt::Permissions *permissions, const char **rootFolder, bool *known);

/**
 * Completes the permissions of a package run on the PC from the "desktop"
 * section of its manifest: root folder (option, then QJSX_ROOT_FOLDER, then
 * the manifest), variables, granted kinds. 'programFolder' is
 * ${executableFolder} (the executable's folder, or the package file's for
 * "qjsx run app.qjspkg"). False after printing why: the run must not start.
 */
bool applyDesktopSection(const uint8_t *package, size_t size, const char *programFolder, const char *rootOption,
                         rt::Permissions *permissions);

/**
 * The root folder of a script run ("qjsx run script.js"): the option, else
 * QJSX_ROOT_FOLDER, else none (the system's current folder). False after
 * printing why.
 */
bool applyScriptRoot(const char *rootOption, rt::Permissions *permissions);

/**
 * The folder of the file 'path' (its real path without the last component;
 * malloc'd, freed by the caller), or nullptr.
 */
char *folderOf(const char *path);

} // namespace host
