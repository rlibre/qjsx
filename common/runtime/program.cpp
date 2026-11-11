/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

#include "program.h"
#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "files.h"
#include "jsbind.h"
#include "loop.h"
#include "modules.h"

namespace rt {

namespace {

/**
 * What the module loader needs during a run: the package (its bytecode
 * modules) and the hooks (notification, extra loader).
 */
struct Session {
    const package::Reader *package;   // may be null
    const RunHooks *hooks;            // may be null
};

/**
 * Calls hooks->moduleLoaded if there is one.
 */
void notifyModuleLoaded(const RunHooks *hooks, const char *name) {
    bool wanted = hooks != nullptr && hooks->moduleLoaded != nullptr;
    if (wanted) {
        hooks->moduleLoaded(hooks->opaque, name);
    }
}

/**
 * Bytecode entry of a module in the package ("bytecode/<name>.qbc"), or
 * nullptr.
 */
const package::Entry *findBytecode(const package::Reader *reader, const char *name) {
    // build the entry name: 16 bytes cover the prefix, the suffix and the
    // terminator
    size_t size = strlen(name) + 16;
    char *entryName = (char *)memory::allocate(size);
    snprintf(entryName, size, "bytecode/%s.qbc", name);

    // look it up
    const package::Entry *entry = reader->find(entryName);
    memory::release(entryName);
    return entry;
}

/**
 * Reads a module of the package from its bytecode, in place. nullptr with a
 * pending exception if the bytecode is invalid.
 */
JSModuleDef *readPackageModule(JSContext *ctx, const Session *session, const package::Entry *entry, const char *name) {
    // read the bytecode: JS_ReadObject registers the module in the context
    JSValue module = JS_ReadObject(ctx, entry->data, entry->size, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(module)) {
        return nullptr;
    }

    // set import.meta, then drop the returned value: the context keeps the
    // module
    JSModuleDef *definition = (JSModuleDef *)JS_VALUE_GET_PTR(module);
    setImportMeta(ctx, definition, name, false);
    JS_FreeValue(ctx, module);

    // tell the hooks while the module is not evaluated yet: the debugger
    // sets its stops before its code runs
    notifyModuleLoaded(session->hooks, name);
    return definition;
}

/**
 * Module loader of the runtime. Returns nullptr with a pending exception if
 * the module is found nowhere.
 */
JSModuleDef *loadModule(JSContext *ctx, const char *name, void *opaque, JSValueConst) {
    Session *session = (Session *)opaque;

    // a native module (C++) first
    const NativeModule *native = findNativeModule(name);
    if (native != nullptr) {
        return native->create(ctx, name);
    }

    // then a module of the package being run
    if (session->package != nullptr) {
        const package::Entry *entry = findBytecode(session->package, name);
        if (entry != nullptr) {
            return readPackageModule(ctx, session, entry, name);
        }
    }

    // then a JS module embedded in the executable, compiled here
    const EmbeddedModule *embedded = findEmbeddedModule(name);
    if (embedded != nullptr) {
        return compileModule(ctx, name, embedded->source, strlen(embedded->source));
    }

    // then the extra loader of the hooks (host: files on disk)
    bool hasExtraLoader = session->hooks != nullptr && session->hooks->loadModule != nullptr;
    if (hasExtraLoader) {
        return session->hooks->loadModule(ctx, name, session->hooks->opaque);
    }

    // found nowhere
    JS_ThrowReferenceError(ctx, "could not load module '%s'", name);
    return nullptr;
}

/**
 * The package manifest parsed as a JS object, or JS_EXCEPTION after printing
 * why.
 */
JSValue readManifest(JSContext *ctx, const package::Reader *reader) {
    // find manifest.json
    const package::Entry *manifest = reader->find("manifest.json");
    if (manifest == nullptr) {
        fprintf(stderr, "qjsx: the package has no manifest.json\n");
        return JS_EXCEPTION;
    }

    // copy it zero-terminated: JS_ParseJSON needs it so
    char *text = (char *)memory::allocate(manifest->size + 1);
    memcpy(text, manifest->data, manifest->size);
    text[manifest->size] = 0;

    // parse it
    JSValue parsed = JS_ParseJSON(ctx, text, manifest->size, "manifest.json");
    memory::release(text);
    if (JS_IsException(parsed)) {
        js::reportException(ctx);
    }

    return parsed;
}

/**
 * "main" of the parsed manifest (malloc'd, freed by the caller), or nullptr
 * after printing why.
 */
char *manifestMain(JSContext *ctx, JSValueConst manifest) {
    // read "main" as a C string (nullptr if it is not a string, or if the
    // conversion fails: out of JS memory)
    JSValue main = JS_GetPropertyStr(ctx, manifest, "main");
    bool isString = JS_IsString(main);
    const char *mainName = isString ? JS_ToCString(ctx, main) : nullptr;

    // copy it out of the JS value, or say why there is none
    char *result = nullptr;
    if (mainName != nullptr) {
        result = (char *)memory::allocate(strlen(mainName) + 1);
        strcpy(result, mainName);
    } else if (isString) {
        fprintf(stderr, "qjsx: cannot read \"main\" of manifest.json (out of memory)\n");
    } else {
        fprintf(stderr, "qjsx: manifest.json has no \"main\"\n");
    }

    // free the JS values
    JS_FreeCString(ctx, mainName);
    JS_FreeValue(ctx, main);
    return result;
}

/**
 * True if the package holds the bytecode this engine reads ("bytecode" of
 * the parsed manifest, written by qjsx build); prints why if not. An engine
 * only reads the bytecode of its own version: checked here, before any
 * module is loaded, the error is clear.
 */
bool bytecodeMatches(JSContext *ctx, JSValueConst manifest) {
    // read "bytecode" as a number (absent in a package of an older qjsx)
    JSValue field = JS_GetPropertyStr(ctx, manifest, "bytecode");
    int32_t packaged = 0;
    bool present = JS_IsNumber(field) && JS_ToInt32(ctx, &packaged, field) == 0;
    JS_FreeValue(ctx, field);
    if (!present) {
        fprintf(stderr, "qjsx: the package does not say which bytecode version it holds: rebuild it\n");
        return false;
    }

    // compare it with the version of this engine
    int expected = JS_GetBytecodeVersion();
    if (packaged != expected) {
        fprintf(stderr, "qjsx: the package holds bytecode version %d, this engine reads version %d: rebuild it\n",
                (int)packaged, expected);
        return false;
    }

    return true;
}

/**
 * Copies the string properties of 'from' into 'to' (other values skipped).
 */
void copyStrings(JSContext *ctx, JSValueConst from, JSValueConst to) {
    // the own enumerable string-named properties
    JSPropertyEnum *names = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &names, &count, from, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }

    // copy each string value
    for (uint32_t i = 0; i < count; i++) {
        JSValue value = JS_GetProperty(ctx, from, names[i].atom);
        if (JS_IsString(value)) {
            JS_SetProperty(ctx, to, names[i].atom, value);
        } else {
            JS_FreeValue(ctx, value);
        }
    }

    JS_FreePropertyEnum(ctx, names, count);
}

/**
 * Device run (RunHooks::device): process.argv = ["qjsx", main module] and
 * process.env = the strings of the manifest's "env" ({} without one).
 */
void setDeviceProcess(JSContext *ctx, JSValueConst manifest, const char *main) {
    // the process object made by installGlobals
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue process = JS_GetPropertyStr(ctx, global, "process");

    // argv: the tool and the main module, like "node main.js"; nothing else
    // (no command line on a device)
    JSValue argv = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, argv, 0, JS_NewString(ctx, "qjsx"));
    JS_SetPropertyUint32(ctx, argv, 1, JS_NewString(ctx, main));
    JS_SetPropertyStr(ctx, process, "argv", argv);

    // env: the "env" of qjsx.json, copied into the manifest by qjsx build
    // (never the environment of the machine running the agent)
    JSValue env = JS_NewObject(ctx);
    JSValue packaged = JS_GetPropertyStr(ctx, manifest, "env");
    if (JS_IsObject(packaged)) {
        copyStrings(ctx, packaged, env);
    }

    JS_SetPropertyStr(ctx, process, "env", env);

    // free the values read
    JS_FreeValue(ctx, packaged);
    JS_FreeValue(ctx, process);
    JS_FreeValue(ctx, global);
}

/**
 * What startPackage() needs: the package and the hooks.
 */
struct PackageStart {
    const package::Reader *reader;  // borrowed
    const RunHooks *hooks;          // may be null
};

/**
 * StartFunction of a package run (startOpaque: a PackageStart): reads,
 * resolves and evaluates the main module. False after printing the error.
 */
bool startPackage(JSContext *ctx, void *startOpaque) {
    const PackageStart *start = (const PackageStart *)startOpaque;

    // parse the manifest
    JSValue manifest = readManifest(ctx, start->reader);
    if (JS_IsException(manifest)) {
        return false;
    }

    // a package compiled for another engine cannot run
    if (!bytecodeMatches(ctx, manifest)) {
        JS_FreeValue(ctx, manifest);
        return false;
    }

    // read the name of the main module (freed on every path below)
    char *main = manifestMain(ctx, manifest);

    // a device run: argv and env come from the manifest, before any code runs
    bool isDevice = start->hooks != nullptr && start->hooks->device;
    if (main != nullptr && isDevice) {
        setDeviceProcess(ctx, manifest, main);
    }

    JS_FreeValue(ctx, manifest);
    if (main == nullptr) {
        return false;
    }

    // find its bytecode
    const package::Entry *entry = findBytecode(start->reader, main);
    if (entry == nullptr) {
        fprintf(stderr, "qjsx: no bytecode for the main module %s\n", main);
        memory::release(main);
        return false;
    }

    // read the main module; the debugger is told before its imports are
    // loaded: their own notifications come after
    JSValue module = JS_ReadObject(ctx, entry->data, entry->size, JS_READ_OBJ_BYTECODE);
    if (!JS_IsException(module)) {
        notifyModuleLoaded(start->hooks, main);
    }

    // resolve the imports (the loader reads them from the package)
    bool ok = !JS_IsException(module) && JS_ResolveModule(ctx, module) >= 0;
    if (!ok) {
        JS_FreeValue(ctx, module);
        js::reportException(ctx);
        memory::release(main);
        return false;
    }

    // evaluate the main module
    ok = evaluateMain(ctx, module, main);
    memory::release(main);
    return ok;
}

/**
 * The runtime of a run: JS runtime, context and loop (created by
 * createRuntime(), freed by destroyRuntime()).
 */
struct Runtime {
    JSRuntime *runtime;  // freed by destroyRuntime()
    JSContext *ctx;      // freed by destroyRuntime()
    Loop *loop;          // current loop of the thread; freed by destroyRuntime()
};

/**
 * Creates the runtime, the context and the loop of a run. 'session' must
 * outlive the runtime: the module loader reads it.
 */
Runtime createRuntime(Session *session) {
    // the runtime on the heap of the board (PSRAM on the targets) or the
    // default one, then the context
    Runtime created;
    const JSMallocFunctions *allocator = board::jsAllocator();
    created.runtime = allocator != nullptr ? JS_NewRuntime2(allocator, nullptr) : JS_NewRuntime();
    created.ctx = JS_NewContext(created.runtime);

    // the stack limit of the board (a task stack of a few dozen KB on a
    // target), measured from here, on the JS thread
    size_t stackSize = board::jsStackSize();
    if (stackSize > 0) {
        JS_SetMaxStackSize(created.runtime, stackSize);
    }

    // install the module loader, then create the loop (it becomes the
    // current loop of the thread)
    JS_SetModuleLoaderFunc2(created.runtime, nullptr, &loadModule, nullptr, session);
    created.loop = new Loop(created.ctx);
    return created;
}

/**
 * Frees what createRuntime() created, in the reverse order.
 */
void destroyRuntime(Runtime runtime) {
    // release the objects kept alive (pending I/O), so that they can be freed
    js::releaseAllKeptAlive();

    // the loop releases its JS values (timers, rejections) while the context
    // still exists
    runtime.loop->shutdown();

    // free the context, then the runtime: finalizers of native objects may
    // still remove their loop sources
    JS_FreeContext(runtime.ctx);
    JS_FreeRuntime(runtime.runtime);

    // so the loop object goes last; it no longer touches JS
    delete runtime.loop;
}

} // namespace

/**
 * Sets import.meta.url and import.meta.main of a module.
 */
void setImportMeta(JSContext *ctx, JSModuleDef *module, const char *name, bool isMain) {
    // get the import.meta object; on error the module just lacks its meta
    // (the error is cleared)
    JSValue meta = JS_GetImportMeta(ctx, module);
    if (JS_IsException(meta)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }

    // build the URL: "file://" before an absolute POSIX path, "file:///"
    // before a Windows drive path, nothing before a package name
    size_t size = strlen(name) + 16;
    char *url = (char *)memory::allocate(size);
    bool windowsAbsolute = name[0] != 0 && name[1] == ':';
    const char *scheme = name[0] == '/' ? "file://" : windowsAbsolute ? "file:///" : "";
    snprintf(url, size, "%s%s", scheme, name);
    files::useSlashes(url);   // a URL has no '\' (Windows)

    // set "url" and "main"
    JS_SetPropertyStr(ctx, meta, "url", JS_NewString(ctx, url));
    JS_SetPropertyStr(ctx, meta, "main", JS_NewBool(ctx, isMain));
    memory::release(url);
    JS_FreeValue(ctx, meta);
}

/**
 * Compiles a module without evaluating it: it is registered in the context
 * and evaluated later, with its importer.
 */
JSModuleDef *compileModule(JSContext *ctx, const char *name, const char *source, size_t size) {
    // compile only: the module is registered in the context
    JSValue compiled = JS_Eval(ctx, source, size, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) {
        return nullptr;
    }

    // set import.meta, then drop the returned value: the context keeps the
    // module
    JSModuleDef *module = (JSModuleDef *)JS_VALUE_GET_PTR(compiled);
    setImportMeta(ctx, module, name, false);
    JS_FreeValue(ctx, compiled);
    return module;
}

/**
 * Marks the module as main in import.meta and evaluates it ('module' is
 * consumed). False after printing a synchronous failure; true when the
 * module called process.exit (the loop then ends at once with its code).
 */
bool evaluateMain(JSContext *ctx, JSValue module, const char *name) {
    // mark it main, then evaluate it (JS_EvalFunction consumes it)
    setImportMeta(ctx, (JSModuleDef *)JS_VALUE_GET_PTR(module), name, true);
    JSValue result = JS_EvalFunction(ctx, module);

    // process.exit() during the evaluation: its exception only unwound the
    // module, it is not an error
    bool exited = Loop::current()->exitRequested();
    if (JS_IsException(result) && exited) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return true;
    }

    // a synchronous failure is printed here
    if (JS_IsException(result)) {
        js::reportException(ctx);
        return false;
    }

    // drop the returned promise: a later rejection reaches the loop's
    // rejection tracker
    JS_FreeValue(ctx, result);
    return true;
}

/**
 * Runs a program, from the creation of its runtime to its destruction.
 * Returns the exit code.
 */
int runProgram(const package::Reader *package, StartFunction *start, void *startOpaque, const RunHooks *hooks,
               int argumentCount, char **arguments) {
    // create the runtime, the context and the loop; set process.argv
    Session session = { package, hooks };
    Runtime runtime = createRuntime(&session);
    setProcessArguments(argumentCount, arguments);

    // apply the hooks, and give them the loop (agent: stop requests,
    // debugger)
    if (hooks != nullptr) {
        runtime.loop->setExitOnUncaught(hooks->exitOnUncaught);
        if (hooks->interrupt != nullptr) {
            JS_SetInterruptHandler(runtime.runtime, hooks->interrupt, hooks->opaque);
        }

        if (hooks->started != nullptr) {
            hooks->started(hooks->opaque, runtime.loop);
        }
    }

    // install the globals, start the main module, then run the event loop
    // until nothing is left to do (or exit)
    bool started = installGlobals(runtime.ctx) && start(runtime.ctx, startOpaque);
    int exitCode = started ? runtime.loop->run() : 1;

    // tell the hooks the loop has ended (the runtime still exists)
    if (hooks != nullptr && hooks->finished != nullptr) {
        hooks->finished(hooks->opaque);
    }

    // free everything
    destroyRuntime(runtime);
    return exitCode;
}

/**
 * Runs the main module of a package held in memory, read in place. Returns
 * the exit code (1 if the package is invalid).
 */
int runPackage(const uint8_t *data, size_t size, const RunHooks *hooks, int argumentCount, char **arguments) {
    // parse the package in place; the reader lives on this stack frame for
    // the whole run
    package::Reader reader;
    if (!reader.open(data, size)) {
        fprintf(stderr, "qjsx: invalid package: %s\n", reader.error());
        return 1;
    }

    // run it, with startPackage() reading the main module from it
    PackageStart start = { &reader, hooks };
    return runProgram(&reader, &startPackage, &start, hooks, argumentCount, arguments);
}

} // namespace rt
