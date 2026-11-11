// espjs: host tool of the project.
//   espjs run <file.js | package.qjspkg> [arguments...]
//   espjs --version
//   espjs <command> ...   other commands, written in JS (host/js/cli.js)
#include <stdio.h>
#include <string.h>

#include "files.h"
#include "memory.h"
#include "host.h"
#include "version.h"
#include "modules.h"

/**
 * Creates the native module "compiler" (compiler_module.cpp, JS_MODULE).
 */
JSModuleDef *jsNewModule_compiler(JSContext *ctx, const char *moduleName);

/**
 * Creates the native module "net" (net.cpp, JS_MODULE).
 */
JSModuleDef *jsNewModule_net(JSContext *ctx, const char *moduleName);

/**
 * Creates the native module "serial" (serial_module.cpp, JS_MODULE).
 */
JSModuleDef *jsNewModule_serial(JSContext *ctx, const char *moduleName);

/**
 * "espjs agent ..." (agent_command.cpp). Returns the exit code.
 */
int runAgentCommand(int argc, char **argv);

#ifdef ESPJS_TESTS
/**
 * Registers the native test modules (tests/native, built into espjs-tests only).
 */
void registerTestModules();
#endif

namespace {

/**
 * "espjs run <path> [arguments...]": runs the file with process.argv =
 * [espjs, path, arguments...], like "node path arguments...". Returns the exit code.
 */
int runScript(int argc, char **argv) {
    // the script's path, process.argv[1], with '/' separators like argv[0]
    // (changed in place, as main() does for argv[0]); the arguments after it
    // belong to the script and stay as they are
    files::useSlashes(argv[2]);

    // the new command line is the old one without the word "run", on the
    // heap (no array on the stack)
    int count = argc - 1;
    char **arguments = (char **)memory::allocate(sizeof(char *) * count);

    // keep the executable, skip "run", keep the path and the arguments
    arguments[0] = argv[0];

    for (int i = 2; i < argc; i++) {
        arguments[i - 1] = argv[i];
    }

    // run; process.argv was copied into JS strings at the start, the array
    // can go
    int exitCode = host::runFile(argv[2], count, arguments);
    memory::release(arguments);
    return exitCode;
}

} // namespace

/**
 * Entry point of the tool: runs the command of the command line, returns its exit code.
 */
int main(int argc, char **argv) {
    // process.argv[0] is the full path of the executable, as in Node: the
    // tool finds stdlib.qjslib next to it (host/js/library.js). The string
    // lives as long as the process. If the system cannot tell the path, the
    // name typed on the command line stays
    char *executable = host::executablePath();
    if (executable != nullptr) {
        argv[0] = executable;
    }

    // with '/' separators on Windows, like every path the runtime gives to
    // JS (C allows changing the argv strings in place)
    files::useSlashes(argv[0]);

    // register the native modules of the host, before any JS runtime is
    // created (espjs-target is linked with the engine without compiler,
    // premake5.lua; the test modules exist in espjs-tests only)
#ifndef ESPJS_NO_COMPILER
    rt::registerNativeModule("compiler", &jsNewModule_compiler);
#endif
    rt::registerNativeModule("net", &jsNewModule_net);
    rt::registerNativeModule("serial", &jsNewModule_serial);
#ifdef ESPJS_TESTS
    registerTestModules();
#endif

    // commands written in C++: --version
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("espjs %s (QuickJS %s)\n", ESPJS_VERSION, CONFIG_VERSION);
        return 0;
    }

    // run
    if (argc >= 3 && strcmp(argv[1], "run") == 0) {
        return runScript(argc, argv);
    }

    // agent: the desktop device agent runs JS runtimes itself
    if (argc >= 2 && strcmp(argv[1], "agent") == 0) {
        return runAgentCommand(argc, argv);
    }

    // every other command (build, help...) is handled by the JS part of the
    // tool ("espjs:cli")
    return host::runCommand(argc, argv);
}
