// Native and embedded modules of the runtime, and the global objects.
#pragma once

#include "quickjs.h"

namespace rt {

typedef JSModuleDef *NativeModuleFactory(JSContext *ctx, const char *moduleName);

struct NativeModule {
    const char          *name;    // import name ("fs"); static text
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
 * Native module registered under this name ("fs"...), or nullptr.
 */
const NativeModule *findNativeModule(const char *name);

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
 * Runs the programs as on a device (the agent calls it once, before any run):
 * process.cwd() is 'directory' and fs resolves relative paths against it
 * ('directory' borrowed, must stay valid; absolute, '/' separators). nullptr:
 * the board has no file system: process.cwd() is "/" and fs fails with ENOSYS.
 * Without this call (host tool), the system's current directory is used.
 */
void setDataDirectory(const char *directory);

/**
 * The path fs must open for 'path' given by the program (malloc'd, freed by
 * the caller): a copy of it, or, on a device, a relative path joined to the
 * data directory. nullptr with errno ENOSYS on a device without file system.
 */
char *resolveProgramPath(const char *path);

} // namespace rt
