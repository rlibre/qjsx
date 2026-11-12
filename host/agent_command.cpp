/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// qjsx agent: the device side running on the desktop, a stand-in for a
// board. The same code as the firmware (agent::runDevice, common/agent),
// with the desktop board (host/platform/board_host.cpp): packages in files,
// a TCP link, the JS program on the main thread.
//
//   qjsx agent [--host 127.0.0.1] [--port 5555] [--storage build/agent] [--token secret]
//              [--board <name | file.json>] [-p | --project <folder>] [--ui-port 8090]
//              [--usb-port <port>] [--uart-port <port>]
//
// With --board, the agent is the digital twin of that board (host/twin/sim/twin.h):
// a program sees the board's native modules (device/gpio, device/rmt,
// device/usb, device/i2c and device/serial simulated) and its
// process.platform, and a web page shows the pins, the parts and the UARTs
// (host/twin/js/server.js, run by a JS runtime of its own on a thread of its
// own). The serial functions of device/usb and the UARTs of device/serial
// are TCP links of the PC (host/twin/sim/twin_tcp.h): the page chooses
// their ports and turns them on, and the twin keeps the choices in
// <storage>/twin.json; nothing listens the first time. --usb-port and
// --uart-port make them listen from the start instead (function n of the
// USB device, UART n, on that port + n; tests and scripts), without saving.
//
// The twin of a project: "qjsx agent" in the folder of a project whose
// qjsx.json has a "twin" section (or "qjsx agent -p <folder>" from
// elsewhere) is the twin of the board it names, with the parts it wires
// ("parts": their models run in the twin's runtime, qjsx:twin/parts) and its
// ports ("ports": { agent, ui, usb, uart }, each one unless given on the
// command line). The program starts once every part is built; a part that
// cannot be built stops the agent. --board on the command line wins over
// the project's board; the project's parts come with -p only then.
//
// Hidden option, to work on the page (host/twin/ui): --ui-dir <folder> serves
// the page from that folder (its dist/, built by "npm run build") instead of
// the one embedded in the executable.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_host.h"
#include "device_modules.h"
#include "files.h"
#include "host.h"
#include "memory.h"
#include "modules.h"
#include "platform.h"
#include "quickjs.h"
#include "runner.h"
#include "twin.h"
#include "version.h"

/**
 * Creates the module "qjsx:twinboard" (host/twin/sim/twin_module.cpp, JS_MODULE).
 */
JSModuleDef *jsNewModule_twinboard(JSContext *ctx, const char *moduleName);

namespace {

/**
 * Options of "qjsx agent", with their defaults.
 */
struct Options {
    const char *host = "127.0.0.1";       // address to listen on (borrowed from argv)
    int         port = 5555;              // TCP port to listen on
    const char *storage = "build/agent";  // directory of the installed package (borrowed from argv, or owned)
    const char *token = nullptr;          // token the host must give in Hello; nullptr: none
    const char *board = nullptr;          // twin of this board (name or .json path); nullptr: none
    const char *project = nullptr;        // folder of the project of the twin (-p, or "."); nullptr: none
    int         uiPort = 8090;            // TCP port of the twin's page
    int         usbPort = 0;              // TCP port of serial function 0 of the twin's USB device; 0: the page's choice
    int         uartPort = 0;             // TCP port of UART 0 of the twin; 0: the page's choice
    const char *uiDir = nullptr;          // hidden: folder the page is served from; nullptr: the embedded one
    bool        portGiven = false;        // the command line gave --port (wins over the project's)
    bool        uiPortGiven = false;      // ... --ui-port
    bool        usbPortGiven = false;     // ... --usb-port
    bool        uartPortGiven = false;    // ... --uart-port
    bool        storageGiven = false;     // ... --storage
    bool        projectGiven = false;     // ... -p / --project
};

const size_t UI_STACK = 4 * 1024 * 1024;  // stack of the twin's page thread (QuickJS lets JS use 1 MB)
const int UI_WORDS = 10;                  // most words of the page thread's command line
const int64_t READY_TIMEOUT_MS = 20000;   // time the twin's runtime has to build the parts
const char *const PROJECT_FILE = "qjsx.json";

const char *const USAGE = "usage: qjsx agent [--host 127.0.0.1] [--port 5555] [--storage build/agent] "
                          "[--token secret] [--board <name | file.json>] [-p | --project <folder>] [--ui-port 8090] "
                          "[--usb-port <port>] [--uart-port <port>]\n";

// The command line of the twin's page thread ("qjsx twin-ui ..."), kept for
// the life of the process.
struct UiArguments {
    char *words[UI_WORDS];  // argv: executable, "twin-ui", "--host", host, "--port", port
                            // [, "--project", folder] [, "--ui-dir", folder] (borrowed)
    int   count;            // words used
    char  port[16];         // the port as text
};

// The "twin" section of a project's qjsx.json, as the agent needs it.
struct ProjectTwin {
    bool  found;            // the project has a "twin" section
    char *board;            // its "board": a name, or a .json path from the project's folder; owned
    int   agentPort;        // "ports.agent"; 0: not given
    int   uiPort;           // "ports.ui"; 0: not given
    int   usbPort;          // "ports.usb"; 0: not given
    int   uartPort;         // "ports.uart"; 0: not given
};

/**
 * Reads the options that follow "qjsx agent" into *options (missing ones keep
 * their default). Prints the usage and returns false on a bad option.
 */
bool parseOptions(int argc, char **argv, Options *options) {
    // each option takes the next word as its value; an unknown option or a
    // missing value is a usage error
    for (int i = 2; i < argc; i++) {
        bool hasValue = i + 1 < argc;
        bool project = strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--project") == 0;
        if (strcmp(argv[i], "--host") == 0 && hasValue) {
            options->host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && hasValue) {
            options->port = atoi(argv[++i]);
            options->portGiven = true;
        } else if (strcmp(argv[i], "--storage") == 0 && hasValue) {
            options->storage = argv[++i];
            options->storageGiven = true;
        } else if (strcmp(argv[i], "--token") == 0 && hasValue) {
            options->token = argv[++i];
        } else if (strcmp(argv[i], "--board") == 0 && hasValue) {
            options->board = argv[++i];
        } else if (project && hasValue) {
            options->project = argv[++i];
            options->projectGiven = true;
        } else if (strcmp(argv[i], "--ui-port") == 0 && hasValue) {
            options->uiPort = atoi(argv[++i]);
            options->uiPortGiven = true;
        } else if (strcmp(argv[i], "--usb-port") == 0 && hasValue) {
            options->usbPort = atoi(argv[++i]);
            options->usbPortGiven = true;
        } else if (strcmp(argv[i], "--uart-port") == 0 && hasValue) {
            options->uartPort = atoi(argv[++i]);
            options->uartPortGiven = true;
        } else if (strcmp(argv[i], "--ui-dir") == 0 && hasValue) {
            options->uiDir = argv[++i];
        } else {
            fputs(USAGE, stderr);
            return false;
        }
    }

    return true;
}

/**
 * "<folder>/<name>", owned by the caller.
 */
char *joinPath(const char *folder, const char *name) {
    size_t size = strlen(folder) + 1 + strlen(name) + 1;
    char *path = (char *)memory::allocate(size);
    snprintf(path, size, "%s/%s", folder, name);
    return path;
}

/**
 * The integer property 'name' of 'object' into *out, when it is one from 1
 * to 65535 (a port); false (after printing why) when it is something else.
 * A missing property leaves *out alone.
 */
bool readPort(JSContext *ctx, JSValueConst object, const char *name, const char *file, int *out) {
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    bool missing = JS_IsUndefined(value);
    int32_t port = 0;
    bool valid = JS_IsNumber(value) && JS_ToInt32(ctx, &port, value) == 0 && port >= 1 && port <= 65535;
    JS_FreeValue(ctx, value);
    if (missing) {
        return true;
    }

    if (!valid) {
        fprintf(stderr, "qjsx agent: %s: \"twin.ports.%s\" must be a port (1 to 65535)\n", file, name);
        return false;
    }

    *out = port;
    return true;
}

/**
 * Reads the "twin" section of the qjsx.json of 'folder' into *twin
 * (twin->found false when the file or the section is missing). False after
 * printing why when the file cannot be parsed or the section is wrong; the
 * parts are checked by the twin's runtime (host/twin/js/parts.js).
 */
bool readProjectTwin(const char *folder, ProjectTwin *twin) {
    *twin = ProjectTwin{};

    // the file, if any
    char *file = joinPath(folder, PROJECT_FILE);
    size_t size = 0;
    uint8_t *text = files::readAll(file, &size);
    if (text == nullptr) {
        memory::release(file);
        return true;
    }

    // parse it with a short-lived context
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(runtime);
    JSValue settings = JS_ParseJSON(ctx, (const char *)text, size, file);
    memory::release(text);
    JSValue section = JS_IsObject(settings) ? JS_GetPropertyStr(ctx, settings, "twin") : JS_UNDEFINED;
    bool ok = JS_IsObject(settings);
    if (!ok) {
        fprintf(stderr, "qjsx agent: %s is not a JSON object\n", file);
    }

    // the section: "board" required, "ports" optional
    if (ok && !JS_IsUndefined(section)) {
        JSValue board = JS_GetPropertyStr(ctx, section, "board");
        const char *name = JS_IsString(board) ? JS_ToCString(ctx, board) : nullptr;
        ok = name != nullptr && name[0] != 0;
        if (!ok) {
            fprintf(stderr, "qjsx agent: %s: \"twin.board\" (the board of the twin) is required\n", file);
        } else {
            twin->found = true;
            twin->board = memory::copyText(name, strlen(name));
        }

        JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, board);

        JSValue ports = JS_GetPropertyStr(ctx, section, "ports");
        if (ok && JS_IsObject(ports)) {
            ok = readPort(ctx, ports, "agent", file, &twin->agentPort) && readPort(ctx, ports, "ui", file, &twin->uiPort) &&
                 readPort(ctx, ports, "usb", file, &twin->usbPort) && readPort(ctx, ports, "uart", file, &twin->uartPort);
        }

        JS_FreeValue(ctx, ports);
    }

    JS_FreeValue(ctx, section);
    JS_FreeValue(ctx, settings);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    memory::release(file);
    return ok;
}

/**
 * Makes the agent the twin of its project, when there is one (see the top
 * of the file):
 * 1. the project: -p, else the current folder (its qjsx.json is optional
 *    then, and --board alone means no project);
 * 2. its "twin" section: the board (unless --board), the ports not given on
 *    the command line, the storage in the project when -p;
 * 3. the folder kept for the twin's runtime (the parts).
 * False after printing why.
 */
bool applyProject(Options *options) {
    // a project only with -p, or without --board
    if (!options->projectGiven && options->board != nullptr) {
        return true;
    }

    const char *folder = options->projectGiven ? options->project : ".";
    ProjectTwin twin;
    if (!readProjectTwin(folder, &twin)) {
        return false;
    }

    // no twin section: a plain agent, unless -p asked for a twin
    if (!twin.found) {
        if (options->projectGiven) {
            fprintf(stderr, "qjsx agent: %s/%s has no \"twin\" section\n", folder, PROJECT_FILE);
            return false;
        }
        return true;
    }

    // the board: a name, or a description file of the project
    size_t length = strlen(twin.board);
    bool isFile = length > 5 && strcmp(twin.board + length - 5, ".json") == 0;
    if (options->board == nullptr) {
        options->board = isFile && !files::isAbsolute(twin.board) ? joinPath(folder, twin.board) : twin.board;
    }

    // the ports the command line did not give
    if (!options->portGiven && twin.agentPort != 0) {
        options->port = twin.agentPort;
    }
    if (!options->uiPortGiven && twin.uiPort != 0) {
        options->uiPort = twin.uiPort;
    }
    if (!options->usbPortGiven && twin.usbPort != 0) {
        options->usbPort = twin.usbPort;
    }
    if (!options->uartPortGiven && twin.uartPort != 0) {
        options->uartPort = twin.uartPort;
    }

    // the storage in the project when it is elsewhere
    if (options->projectGiven && !options->storageGiven) {
        options->storage = joinPath(folder, "build/agent");
    }

    options->project = folder;
    return true;
}

/**
 * The description file of a board: 'board' itself if it ends with ".json",
 * else "<folder of the executable>/boards/<board>.json" (premake5.lua copies
 * boards/ there). The result is owned by the caller.
 */
char *boardPath(const char *executable, const char *board) {
    // a file given as is
    size_t length = strlen(board);
    bool isFile = length > 5 && strcmp(board + length - 5, ".json") == 0;
    if (isFile) {
        return memory::copyText(board, length);
    }

    // the folder of the executable ('/' separators: main.cpp made them so)
    const char *slash = strrchr(executable, '/');
    const char *folder = slash != nullptr ? executable : ".";
    size_t folderLength = slash != nullptr ? (size_t)(slash - executable) : 1;

    // <folder>/boards/<board>.json
    size_t size = folderLength + strlen("/boards/") + length + strlen(".json") + 1;
    char *path = (char *)memory::allocate(size);
    snprintf(path, size, "%.*s/boards/%s.json", (int)folderLength, folder, board);
    return path;
}

/**
 * Body of the twin's page thread: the tool's JS command "twin-ui" in a
 * runtime of its own, until the process ends.
 */
void runTwinUi(void *argument) {
    UiArguments *arguments = (UiArguments *)argument;
    host::runCommand(arguments->count, arguments->words);

    // stopped before saying whether the parts are built: the agent stops too
    twin::readyReport("the twin's runtime stopped");
    fprintf(stderr, "qjsx agent: the page of the twin stopped\n");
}

/**
 * Makes this agent the twin of options->board:
 * 1. loads the board;
 * 2. registers its simulated modules and the page's module, keeps only the
 *    board's native modules for the program (before any JS runtime exists:
 *    the registry is not thread-safe);
 * 3. gives device/serial the board's UARTs; sets up the TCP links of the
 *    UARTs and of the USB functions;
 * 4. starts the page on a thread of its own.
 * False after printing why.
 */
bool startTwin(const char *executable, const Options *options) {
    // the board
    char *path = boardPath(executable, options->board);
    bool loaded = twin::load(path);
    memory::release(path);
    if (!loaded) {
        return false;
    }

    // its modules
    rt::registerNativeModule("device/gpio", &jsNewModule_gpio);
    rt::registerNativeModule("device/rmt", &jsNewModule_rmt);
    rt::registerNativeModule("device/usb", &jsNewModule_usb);
    rt::registerNativeModule("device/i2c", &jsNewModule_i2c);
    rt::registerNativeModule("device/adc", &jsNewModule_adc);
    rt::registerNativeModule("qjsx:twinboard", &jsNewModule_twinboard);
    rt::setDeviceNativeModules(twin::natives(), twin::nativeCount());

    // device/serial: the board's UARTs instead of the PC's ports (main.cpp
    // set those)
    setSerialDriver(twin::serialDriver());

    // the TCP links of the UARTs and of the USB functions, with the choices
    // kept in the storage folder
    twin::setupUarts(options->host, options->uartPort);
    twin::setupUsb(options->host, options->usbPort);
    if (!twin::startTcp(options->host, options->storage)) {
        return false;
    }

    // the page's command line, which lives as long as the process: the
    // fixed words, then the project and the folder of the page if any
    UiArguments *arguments = (UiArguments *)memory::allocate(sizeof(UiArguments));
    snprintf(arguments->port, sizeof(arguments->port), "%d", options->uiPort);
    const char *fixed[] = { executable, "twin-ui", "--host", options->host, "--port", arguments->port };
    int count = 0;
    for (const char *word : fixed) {
        arguments->words[count++] = (char *)word;
    }
    if (options->project != nullptr) {
        arguments->words[count++] = (char *)"--project";
        arguments->words[count++] = (char *)options->project;
    }
    if (options->uiDir != nullptr) {
        arguments->words[count++] = (char *)"--ui-dir";
        arguments->words[count++] = (char *)options->uiDir;
    }
    arguments->count = count;

    // the page's thread
    platform::ThreadOptions threadOptions = platform::defaultThreadOptions("twin page");
    threadOptions.stackSize = UI_STACK;
    if (!platform::startThread(&runTwinUi, arguments, threadOptions)) {
        fprintf(stderr, "qjsx agent: cannot start the page of the twin\n");
        return false;
    }

    // the parts built before the program runs
    char *error = nullptr;
    if (!twin::readyWait(READY_TIMEOUT_MS, &error)) {
        fprintf(stderr, "qjsx agent: the twin's runtime did not start\n");
        return false;
    }
    if (error != nullptr) {
        fprintf(stderr, "qjsx agent: %s\n", error);
        memory::release(error);
        return false;
    }

    printf("qjsx agent: twin of %s, page on http://%s:%d/\n", twin::name(), options->host, options->uiPort);
    return true;
}

} // namespace

/**
 * "qjsx agent [options]": runs the desktop agent. Returns the exit code (2 on
 * a usage error, 1 if it could not start); never returns otherwise.
 */
int runAgentCommand(int argc, char **argv) {
    // parse the options
    Options options;
    if (!parseOptions(argc, argv, &options)) {
        return 2;
    }

    // the twin of the project, if there is one
    if (!applyProject(&options)) {
        return 1;
    }

    // the twin of a board, if asked
    if (options.board != nullptr && !startTwin(argv[0], &options)) {
        return 1;
    }

    // configure the desktop board: listen address, storage directory (created
    // here), and the identity of the board of a twin
    bool isTwin = twin::active();
    board::HostOptions boardOptions = { options.host,
                                        options.port,
                                        options.storage,
                                        isTwin ? twin::chip() : nullptr,
                                        isTwin ? twin::name() : nullptr,
                                        isTwin ? twin::platform() : nullptr };
    if (!board::configureHost(boardOptions)) {
        return 1;
    }

    // run the device: the calling thread becomes the JS thread
    const char *agentName = isTwin ? "qjsx twin" : "qjsx desktop agent";
    agent::Identity identity = { agentName, CONFIG_VERSION, options.token, JS_GetBytecodeVersion(), QJSX_VERSION };
    agent::runDevice(identity);
    return 1;   // runDevice returns only when the links could not start
}
