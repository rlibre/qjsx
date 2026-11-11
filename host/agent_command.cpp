// espjs agent: the device side running on the desktop, a stand-in for a
// board. The same code as the firmware (agent::runDevice, common/agent),
// with the desktop board (host/platform/board_host.cpp): packages in files,
// a TCP link, the JS program on the main thread.
//
//   espjs agent [--host 127.0.0.1] [--port 5555] [--storage build/agent] [--token secret]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_host.h"
#include "quickjs.h"
#include "runner.h"
#include "version.h"

namespace {

/**
 * Options of "espjs agent", with their defaults.
 */
struct Options {
    const char *host = "127.0.0.1";       // address to listen on (borrowed from argv)
    int         port = 5555;              // TCP port to listen on
    const char *storage = "build/agent";  // directory of the installed package (borrowed from argv)
    const char *token = nullptr;          // token the host must give in Hello; nullptr: none
};

/**
 * Reads the options that follow "espjs agent" into *options (missing ones keep
 * their default). Prints the usage and returns false on a bad option.
 */
bool parseOptions(int argc, char **argv, Options *options) {
    // each option takes the next word as its value; an unknown option or a
    // missing value is a usage error
    for (int i = 2; i < argc; i++) {
        bool hasValue = i + 1 < argc;
        if (strcmp(argv[i], "--host") == 0 && hasValue) {
            options->host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && hasValue) {
            options->port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--storage") == 0 && hasValue) {
            options->storage = argv[++i];
        } else if (strcmp(argv[i], "--token") == 0 && hasValue) {
            options->token = argv[++i];
        } else {
            fprintf(stderr, "usage: espjs agent [--host 127.0.0.1] [--port 5555] [--storage build/agent] [--token secret]\n");
            return false;
        }
    }

    return true;
}

} // namespace

/**
 * "espjs agent [options]": runs the desktop agent. Returns the exit code (2 on
 * a usage error, 1 if it could not start); never returns otherwise.
 */
int runAgentCommand(int argc, char **argv) {
    // parse the options
    Options options;
    if (!parseOptions(argc, argv, &options)) {
        return 2;
    }

    // configure the desktop board: listen address, storage directory (created here)
    board::HostOptions boardOptions = { options.host, options.port, options.storage };
    if (!board::configureHost(boardOptions)) {
        return 1;
    }

    // run the device: the calling thread becomes the JS thread
    agent::Identity identity = { "espjs desktop agent " ESPJS_VERSION, CONFIG_VERSION, options.token,
                                 JS_GetBytecodeVersion() };
    agent::runDevice(identity);
    return 1;   // runDevice returns only when the links could not start
}
