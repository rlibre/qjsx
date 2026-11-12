/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Native and embedded modules of the runtime, and the global objects.
#pragma once

#include "quickjs.h"

namespace rt {

typedef JSModuleDef *NativeModuleFactory(JSContext *ctx, const char *moduleName);

struct NativeModule {
    const char          *name;    // import name ("sys/fs"); static text
    NativeModuleFactory *create;  // creates the module in a context
};

// Module written in JS and embedded in the executable as source, compiled at
// load time: the desktop tool only (its commands "qjsx:name", and its own
// copy of the standard library, "stdlib/name"). A device has no compiler and
// embeds no JS: its programs bring the library modules they use in their
// package (.qjslib libraries, see host/js/build.js).
struct EmbeddedModule {
    const char *name;    // import name ("stdlib/path"); static text
    const char *source;  // JS source, zero-terminated; static text
};

/**
 * Native module registered under this name ("sys/fs"...), or nullptr.
 */
const NativeModule *findNativeModule(const char *name);

/**
 * Number of native modules registered: the modules a program of this
 * firmware may import (the agent lists them in its answer to HELLO).
 */
int countNativeModules();

/**
 * Name of the native module at 'index' (0 <= index < countNativeModules()),
 * a static text.
 */
const char *nativeModuleNameAt(int index);

/**
 * Restricts the native modules a device program may import (and the agent
 * lists in HELLO) to 'names' (borrowed, 'count' of them; count < 0: all,
 * the default). The digital twin of a board gives the board's list, so that
 * a program sees what the real board has. Before any runtime is created.
 */
void setDeviceNativeModules(const char *const *names, int count);

/**
 * True if a device program may import the native module 'name' (see
 * setDeviceNativeModules).
 */
bool isDeviceNativeModule(const char *name);

/**
 * Embedded JS module registered under this name ("stdlib/path"...), or nullptr.
 */
const EmbeddedModule *findEmbeddedModule(const char *name);

/**
 * Adds a native module (tests, host-only modules...). Not thread-safe:
 * call before creating the runtime.
 */
void registerNativeModule(const char *name, NativeModuleFactory *create);

/**
 * Adds an embedded JS module ('source' must stay valid). Same rules.
 */
void registerEmbeddedModule(const char *name, const char *source);

// Registers an embedded module at program start: the C++ files generated
// from JS files by tools/embed-js.lua declare one such object.
struct EmbeddedRegistration {
    /**
     * Registers the module; runs before main() for a static object.
     */
    EmbeddedRegistration(const char *name, const char *source) {
        registerEmbeddedModule(name, source);
    }
};

/**
 * Installs the global objects on the context: console, timers,
 * TextEncoder / TextDecoder, process.
 */
bool installGlobals(JSContext *ctx);

// Console output of the program (console.*, uncaught exceptions). By default
// stdout / stderr; the device agent sends it to the host instead.
enum OutputStream { OUTPUT_STDOUT = 1, OUTPUT_STDERR = 2 };
typedef void OutputSink(void *opaque, int stream, const char *text, size_t length);

/**
 * Sends the program's output to 'sink' from now on; nullptr goes back to
 * stdout / stderr.
 */
void setOutputSink(OutputSink *sink, void *opaque);

/**
 * Writes text to the program's output (an OutputStream), through the sink
 * if one is set.
 */
void writeOutput(int stream, const char *text, size_t length);

/**
 * Arguments shown as process.argv. 'argv' is kept, not copied: it must stay
 * valid until installGlobals has run (which copies it into JS strings).
 */
void setProcessArguments(int argc, char **argv);

/**
 * The path fs must open for 'path' given by the program (malloc'd, freed by
 * the caller): a copy of it, or a relative path joined to the working folder
 * of the run's permissions (rt::permissions(): a device's data folder, a PC
 * program's root folder). nullptr with errno ENOSYS on a device without file
 * system. Only resolves: the access itself is checked by the caller
 * (Permissions::mayAccess).
 */
char *resolveProgramPath(const char *path);

} // namespace rt
