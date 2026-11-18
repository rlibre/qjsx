/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Runs a JS program in a new runtime: creates the runtime and the context,
// installs the globals, starts the main module, runs the event loop, frees
// everything. Same code on the host and on the targets.
//
// Modules are found, in this order: native modules (C++), the bytecode of
// the package being run, the JS modules embedded in the executable, then
// RunHooks::loadModule (the host tool loads the files on disk there).
// A module imported with { type: "bytes" } is a file of the package
// ("bytes/<name>", a Uint8Array over it), else RunHooks::loadBytes; one
// imported with { type: "image" } the same (converted by 'qjsx build'), else
// RunHooks::loadImage.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"
#include "package.h"

namespace rt {

class Loop;
class Permissions;
struct JsLoadUsage;

// The JS heap of a run (RunHooks::heapUsage), in bytes as QuickJS counts
// them. Written by the JS thread, read by any thread with atomic loads.
struct JsHeapUsage {
    size_t used;   // the JS heap now (0 once the runtime is freed)
    size_t peak;   // the most it used since the run started
};

// Optional control over a run (all fields may be null / false).
struct RunHooks {
    void               *opaque;                  // passed to every hook
    void (*started)(void *opaque, Loop *loop);   // the loop exists, the program starts
    void (*finished)(void *opaque);              // the loop has ended, the runtime is not freed yet
    JSInterruptHandler *interrupt;               // stops runaway JS code (opaque passed)
    bool                exitOnUncaught;          // false: print uncaught exceptions and go on
    // Run as on a device (package runs only): process.argv is ["qjsx", main
    // module] and process.env the "env" object of the manifest ({} without
    // one), instead of the arguments given and the system's environment.
    bool                device;
    // A module of the package was loaded and is not evaluated yet (the
    // debugger sets its stops).
    void (*moduleLoaded)(void *opaque, const char *name);
    // Loads a module that is neither native nor in the package; returns
    // nullptr with a pending exception if it cannot.
    JSModuleDef *(*loadModule)(JSContext *ctx, const char *name, void *opaque);
    // Loads a module imported as bytes (with { type: "bytes" }) that is not
    // in the package, with newBytesModule(); returns nullptr with a pending
    // exception if it cannot.
    JSModuleDef *(*loadBytes)(JSContext *ctx, const char *name, void *opaque);
    // Loads a module imported as an image (with { type: "image" }) that is
    // not in the package: the file converted for the displays, as 'qjsx
    // build' does; returns nullptr with a pending exception if it cannot.
    JSModuleDef *(*loadImage)(JSContext *ctx, const char *name, void *opaque);
    // What the program may reach (files, network, environment, serial
    // ports), those of the thread during the run; borrowed. nullptr: the
    // tool's own commands, everything allowed.
    const Permissions  *permissions;
    // Where the JS heap of the run is measured (set to 0 when the runtime
    // is created); borrowed, must outlive the run. nullptr: not measured.
    // Not measured either on a board whose allocator is QuickJS's default
    // (board::jsAllocator() nullptr).
    JsHeapUsage        *heapUsage;
    // Where the load of the JS thread of the run is published (set to 0
    // when the runtime is created; see LoadMeter in loop.h); borrowed, must
    // outlive the run. nullptr: not published.
    JsLoadUsage        *loadUsage;
};

/**
 * The JS heap of the run of the calling JS thread (JS thread), as measured:
 * nullptr when it is not (no RunHooks::heapUsage, or QuickJS's own
 * allocator). device/board's memory() reads it.
 */
const JsHeapUsage *currentHeapUsage();

// Evaluates the main module of a program. Returns false after reporting the error.
typedef bool StartFunction(JSContext *ctx, void *startOpaque);

/**
 * Runs a program: 'package' (may be null) is searched by the module loader,
 * 'start' evaluates the main module. Returns the exit code.
 */
int runProgram(const package::Reader *package, StartFunction *start, void *startOpaque, const RunHooks *hooks,
               int argumentCount, char **arguments);

/**
 * Runs the main module of a package held in memory (not copied: 'data' must
 * stay valid; on a target it is the flash, read in place).
 */
int runPackage(const uint8_t *data, size_t size, const RunHooks *hooks, int argumentCount, char **arguments);

/**
 * Compiles source code as a module registered in the context under 'name'
 * (embedded modules, files on the host). On an engine without compiler it
 * fails: "eval is not supported". Returns nullptr with a pending exception.
 */
JSModuleDef *compileModule(JSContext *ctx, const char *name, const char *source, size_t size);

/**
 * Creates the module 'name' of an import of bytes (with { type: "bytes" }):
 * its only export, 'default', is a Uint8Array over 'data' (not copied).
 * 'release' frees 'data' once the array is collected, also when this call
 * fails; nullptr: 'data' is not freed (the bytes of the package, which
 * outlives the run). Returns nullptr with a pending exception.
 * The bytes are meant to be read only, but nothing stops a write into the
 * array (the engine has no immutable ArrayBuffer): on a device they lie in
 * flash, a write there is an undetected error.
 */
JSModuleDef *newBytesModule(JSContext *ctx, const char *name, uint8_t *data, size_t size,
                            JSFreeArrayBufferDataFunc *release);

/**
 * import.meta.url and import.meta.main, like Node and Deno.
 */
void setImportMeta(JSContext *ctx, JSModuleDef *module, const char *name, bool isMain);

/**
 * Evaluates a compiled or read main module ('module' is consumed). Its later
 * errors are reported by the loop (rejected evaluation promise).
 */
bool evaluateMain(JSContext *ctx, JSValue module, const char *name);

} // namespace rt
