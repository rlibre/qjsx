/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

#include "host.h"
#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "quickjs.h"
#include "jsbind.h"
#include "files.h"
#include "program.h"

namespace host {

namespace {

/**
 * True if 'text' ends with 'suffix'.
 */
bool endsWith(const char *text, const char *suffix) {
    size_t textLength = strlen(text);
    size_t suffixLength = strlen(suffix);
    return textLength >= suffixLength && strcmp(text + textLength - suffixLength, suffix) == 0;
}

/**
 * Last resort of the module loader on the host (rt::RunHooks::loadModule): a
 * source file on disk, compiled. nullptr with a pending ReferenceError if unreadable.
 */
JSModuleDef *loadFileModule(JSContext *ctx, const char *name, void *) {
    // read the file; the module name is its path
    size_t size = 0;
    uint8_t *source = files::readAll(name, &size);
    if (source == nullptr) {
        JS_ThrowReferenceError(ctx, "could not load module '%s': %s", name, strerror(errno));
        return nullptr;
    }

    // compile it; the source is not needed afterwards
    JSModuleDef *module = rt::compileModule(ctx, name, (const char *)source, size);
    memory::release(source);
    return module;
}

/**
 * Hooks of a run of the host tool: modules may also come from files, and an
 * uncaught exception stops the program.
 */
rt::RunHooks fileHooks() {
    rt::RunHooks hooks = {};
    hooks.exitOnUncaught = true;
    hooks.loadModule = &loadFileModule;
    return hooks;
}

/**
 * Main module given as source text.
 */
struct SourceStart {
    const char *name;     // module name (borrowed)
    const char *source;   // source text, borrowed
    size_t size;          // bytes of source
};

/**
 * StartFunction of runSource(): compiles and evaluates the main module. False
 * (run stopped) on a syntax error, reported here.
 */
bool startSource(JSContext *ctx, void *startOpaque) {
    // compile only: the module must not be evaluated yet
    const SourceStart *start = (const SourceStart *)startOpaque;
    JSValue compiled = JS_Eval(ctx, start->source, start->size, start->name,
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) {
        js::reportException(ctx);
        return false;
    }

    // rt::evaluateMain evaluates it the way every main module is evaluated
    return rt::evaluateMain(ctx, compiled, start->name);
}

/**
 * Runs a program whose main module is the source text 'source' named 'name';
 * imports may load files. 'source' must stay valid during the call. Returns the exit code.
 */
int runSource(const char *name, const char *source, size_t size, int argumentCount, char **arguments) {
    SourceStart start = { name, source, size };
    rt::RunHooks hooks = fileHooks();
    return rt::runProgram(nullptr, &startSource, &start, &hooks, argumentCount, arguments);
}

/**
 * Runs a source file as the main module. Returns the exit code (1 if the
 * file cannot be read).
 */
int runScriptFile(const char *path, int argumentCount, char **arguments) {
    // read the whole source
    size_t size = 0;
    uint8_t *source = files::readAll(path, &size);
    if (source == nullptr) {
        fprintf(stderr, "qjsx: cannot read %s: %s\n", path, strerror(errno));
        return 1;
    }

    // run it; the source is freed once the program has ended
    int exitCode = runSource(path, (const char *)source, size, argumentCount, arguments);
    memory::release(source);
    return exitCode;
}

/**
 * Runs the main module of a .qjspkg file from its bytecode. Returns the exit
 * code (1 if the file cannot be read).
 */
int runPackageFile(const char *path, int argumentCount, char **arguments) {
    // load the whole package into memory
    size_t size = 0;
    uint8_t *data = files::readAll(path, &size);
    if (data == nullptr) {
        fprintf(stderr, "qjsx: cannot read %s: %s\n", path, strerror(errno));
        return 1;
    }

    // rt::runPackage reads the package in place: it is freed only after the run
    rt::RunHooks hooks = fileHooks();
    int exitCode = rt::runPackage(data, size, &hooks, argumentCount, arguments);
    memory::release(data);
    return exitCode;
}

} // namespace

/**
 * "qjsx run <path>": a package runs from its bytecode (like on a target),
 * anything else is a source module.
 */
int runFile(const char *path, int argumentCount, char **arguments) {
    // the path is used with '/' separators (the module resolution works with
    // '/', a Windows path may contain '\')
    char *normalized = memory::copyText(path, strlen(path));
    files::useSlashes(normalized);
    bool isPackage = endsWith(normalized, ".qjspkg");

    // run it as a package or as a source module
    int exitCode = isPackage ? runPackageFile(normalized, argumentCount, arguments)
                             : runScriptFile(normalized, argumentCount, arguments);
    memory::release(normalized);
    return exitCode;
}

/**
 * The commands of the tool: runs the JS modules embedded in the executable
 * ("qjsx:cli" and the modules it imports).
 */
int runCommand(int argumentCount, char **arguments) {
    // a two-line main module imports the CLI and awaits main(process.argv.slice(1))
    const char *source = "import { main } from \"qjsx:cli\";\n"
                         "await main(process.argv.slice(1));\n";
    return runSource("<qjsx>", source, strlen(source), argumentCount, arguments);
}

} // namespace host
