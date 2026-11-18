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
#include "permissions_host.h"
#include "program.h"

namespace host {

namespace {

/**
 * True if 'text' ends with 'suffix'.
 */
bool endsWith(const char *text, const char *suffix) {
    size_t textLength = strlen(text);
    size_t suffixLength = strlen(suffix);
    bool longEnough = textLength >= suffixLength;
    return longEnough && strcmp(text + textLength - suffixLength, suffix) == 0;
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
 * Frees the bytes of a file imported as bytes (read by loadFileBytes), once
 * its array is collected.
 */
void releaseFileBytes(JSRuntime *, void *, void *data) {
    memory::release(data);
}

/**
 * Loader of the imports of bytes on the host (rt::RunHooks::loadBytes), for
 * the sources run by 'qjsx run': the file on disk, read into memory; its
 * array owns that copy. Like code, it is read whatever the permissions of
 * the program (a package holds it). nullptr with a pending ReferenceError if
 * unreadable.
 */
JSModuleDef *loadFileBytes(JSContext *ctx, const char *name, void *) {
    // read the file; the module name is its path
    size_t size = 0;
    uint8_t *data = files::readAll(name, &size);
    if (data == nullptr) {
        JS_ThrowReferenceError(ctx, "could not load bytes '%s': %s", name, strerror(errno));
        return nullptr;
    }

    // the module of its bytes, which it frees
    return rt::newBytesModule(ctx, name, data, size, &releaseFileBytes);
}

/**
 * 'size' bytes as text in hexadecimal, two digits a byte (memory::allocate,
 * zero-terminated).
 */
char *hexText(const uint8_t *bytes, size_t size) {
    const char DIGITS[] = "0123456789abcdef";
    char *text = (char *)memory::allocate(size * 2 + 1);

    for (size_t index = 0; index < size; index++) {
        text[index * 2] = DIGITS[bytes[index] >> 4];
        text[index * 2 + 1] = DIGITS[bytes[index] & 0x0F];
    }

    text[size * 2] = 0;
    return text;
}

/**
 * Loader of the imports of images on the host (rt::RunHooks::loadImage),
 * for the sources run by 'qjsx run': the file on disk, converted for the
 * displays as 'qjsx build' does it. The converter is JS (qjsx:image), so
 * the module is made from a source text that hands it the bytes of the
 * file, in hexadecimal: 'default' is the converted image. Read whatever the
 * permissions of the program, like code. nullptr with a pending exception if
 * the file cannot be read or the module cannot be made.
 */
JSModuleDef *loadFileImage(JSContext *ctx, const char *name, void *) {
    // read the file; the module name is its path
    size_t size = 0;
    uint8_t *data = files::readAll(name, &size);
    if (data == nullptr) {
        JS_ThrowReferenceError(ctx, "could not load the image '%s': %s", name, strerror(errno));
        return nullptr;
    }

    // the source: the converter, given the bytes; a failure says which file
    char *hex = hexText(data, size);
    memory::release(data);
    const char HEAD[] =
        "import { imageFromPng } from \"qjsx:image\";\n"
        "const file = Uint8Array.fromHex(\"";
    const char TAIL[] =
        "\");\n"
        "let image;\n"
        "try {\n"
        "    image = imageFromPng(file);\n"
        "} catch (error) {\n"
        "    throw new Error(`${import.meta.url}: the image cannot be converted: ${error.message}`);\n"
        "}\n"
        "export default image;\n";
    size_t hexSize = size * 2;
    size_t sourceSize = sizeof(HEAD) - 1 + hexSize + sizeof(TAIL) - 1;
    char *source = (char *)memory::allocate(sourceSize + 1);
    memcpy(source, HEAD, sizeof(HEAD) - 1);
    memcpy(source + sizeof(HEAD) - 1, hex, hexSize);
    memcpy(source + sizeof(HEAD) - 1 + hexSize, TAIL, sizeof(TAIL));
    memory::release(hex);

    // compiled as the module of that name
    JSModuleDef *module = rt::compileModule(ctx, name, source, sourceSize);
    memory::release(source);
    return module;
}

/**
 * Hooks of a run of the host tool: modules may also come from files, and an
 * uncaught exception stops the program.
 */
rt::RunHooks fileHooks() {
    // the defaults, plus: stop on an uncaught exception, files as modules,
    // as bytes and as images
    rt::RunHooks hooks = {};
    hooks.exitOnUncaught = true;
    hooks.loadModule = &loadFileModule;
    hooks.loadBytes = &loadFileBytes;
    hooks.loadImage = &loadFileImage;
    return hooks;
}

// Main module given as source text.
struct SourceStart {
    const char *name;     // module name (borrowed)
    const char *source;   // source text, borrowed
    size_t      size;     // bytes of source
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
    // startSource compiles and evaluates it in the new runtime
    SourceStart start = { name, source, size };
    rt::RunHooks hooks = fileHooks();
    return rt::runProgram(nullptr, &startSource, &start, &hooks, argumentCount, arguments);
}

/**
 * Runs a source file as the main module, with the permissions of 'launch'
 * (its options only) and its root folder. Returns the exit code (1 if the
 * file cannot be read or the root folder is wrong).
 */
int runScriptFile(const char *path, int argumentCount, char **arguments, const Launch *launch) {
    // the root folder (process.cwd()) and the hint of the refusals
    if (!applyScriptRoot(launch->rootFolder, launch->permissions)) {
        return 1;
    }

    // read the whole source (code is loaded whatever the permissions, as
    // with Deno: they guard what the program does)
    size_t size = 0;
    uint8_t *source = files::readAll(path, &size);
    if (source == nullptr) {
        fprintf(stderr, "qjsx: cannot read %s: %s\n", path, strerror(errno));
        return 1;
    }

    // run it; the source is freed once the program has ended
    SourceStart start = { path, (const char *)source, size };
    rt::RunHooks hooks = fileHooks();
    hooks.permissions = launch->permissions;
    int exitCode = rt::runProgram(nullptr, &startSource, &start, &hooks, argumentCount, arguments);
    memory::release(source);
    return exitCode;
}

/**
 * Runs the main module of a .qjspkg file from its bytecode;
 * ${executableFolder} is the folder of the file. Returns the exit code (1 if
 * the file cannot be read).
 */
int runPackageFile(const char *path, int argumentCount, char **arguments, const Launch *launch) {
    // load the whole package into memory
    size_t size = 0;
    uint8_t *data = files::readAll(path, &size);
    char *folder = nullptr;
    if (data != nullptr) {
        folder = folderOf(path);
    }

    if (folder == nullptr) {
        fprintf(stderr, "qjsx: cannot read %s: %s\n", path, strerror(errno));
        memory::release(data);
        return 1;
    }

    // rt::runPackage reads the package in place: it is freed only after the run
    int exitCode = runPackageData(data, size, folder, argumentCount, arguments, launch);
    memory::release(folder);
    memory::release(data);
    return exitCode;
}

} // namespace

/**
 * A package in memory, with the hooks of the host (an uncaught exception
 * stops the program) and its permissions: the launch's, plus the "desktop"
 * section of its manifest.
 */
int runPackageData(const uint8_t *data, size_t size, const char *programFolder, int argumentCount, char **arguments,
                   const Launch *launch) {
    // the permissions; wrong ones: nothing runs
    if (!applyDesktopSection(data, size, programFolder, launch->rootFolder, launch->permissions)) {
        return 1;
    }

    // run it
    rt::RunHooks hooks = fileHooks();
    hooks.permissions = launch->permissions;
    return rt::runPackage(data, size, &hooks, argumentCount, arguments);
}

/**
 * "qjsx run <path>": a package runs from its bytecode (like on a target),
 * anything else is a source module.
 */
int runFile(const char *path, int argumentCount, char **arguments, const Launch *launch) {
    // the path is used with '/' separators (the module resolution works with
    // '/', a Windows path may contain '\')
    char *normalized = memory::copyText(path, strlen(path));
    files::useSlashes(normalized);
    bool isPackage = endsWith(normalized, ".qjspkg");

    // run it as a package or as a source module
    int exitCode = 0;
    if (isPackage) {
        exitCode = runPackageFile(normalized, argumentCount, arguments, launch);
    } else {
        exitCode = runScriptFile(normalized, argumentCount, arguments, launch);
    }

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
