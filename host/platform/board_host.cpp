// board.h for the desktop agent. See board_host.h.
#include "board.h"
#include "memory.h"
#include "board_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "agent.h"
#include "files.h"
#include "modules.h"
#include "platform.h"
#include "sockets.h"

namespace board {

namespace {

/**
 * Deletes a file; a file already missing is no error.
 */
bool removeFileIfPresent(const char *path) {
    bool removed = files::removeFile(path);
    return removed || errno == ENOENT;
}

/**
 * Packages in files: <storage>/app.qjspkg and its build id in app.buildid.
 * An upload is kept in memory until it is complete, then written.
 */
class FileStorage : public agent::Storage {
public:
    /**
     * Builds the paths of the two files in 'directory', and of their
     * temporary copies made while installing.
     */
    void setDirectory(const char *directory) {
        snprintf(_packagePath, sizeof(_packagePath), "%s/app.qjspkg", directory);
        snprintf(_buildIdPath, sizeof(_buildIdPath), "%s/app.buildid", directory);
        snprintf(_newPackagePath, sizeof(_newPackagePath), "%s/app.qjspkg.new", directory);
        snprintf(_newBuildIdPath, sizeof(_newBuildIdPath), "%s/app.buildid.new", directory);
    }

    /**
     * Path of the installed package file.
     */
    const char *packagePath() const {
        return _packagePath;
    }

    /**
     * Allocates the in-memory upload buffer of 'size' bytes (an unfinished
     * previous upload is dropped). Always true: out of memory is fatal.
     */
    bool beginUpload(size_t size) override {
        memory::release(_upload);
        _upload = (uint8_t *)memory::allocate(size);
        _uploadSize = size;
        return true;
    }

    /**
     * Copies a chunk into the upload buffer. False if no upload is open or
     * the chunk goes past its end.
     */
    bool writeUpload(size_t offset, const uint8_t *data, size_t size) override {
        // the chunk must fit in the open upload
        if (_upload == nullptr || offset + size > _uploadSize) {
            return false;
        }

        memcpy(_upload + offset, data, size);
        return true;
    }

    /**
     * Installs the uploaded package and its build id so that the installed
     * pair never mismatches, even if the process stops in the middle: at
     * worst the package has no build id, read as unknown (empty). The upload
     * buffer is freed whatever the result.
     */
    bool finishUpload(const char *buildId) override {
        // no upload open: nothing to install
        if (_upload == nullptr) {
            return false;
        }

        // 1. write both new files under temporary names, then free the
        //    buffer; a failure here leaves the installed pair untouched
        bool written = files::writeAll(_newPackagePath, _upload, _uploadSize)
                    && files::writeAll(_newBuildIdPath, buildId, strlen(buildId));
        abortUpload();

        // 2. remove the old build id: from here on, the installed package (the
        //    old one, then the new one) has no build id, which reads as
        //    unknown, never as the id of the other package
        // 3. move the new package in place (the rename replaces the old one)
        // 4. move the new build id in place: the pair is complete again
        bool isInstalled = written
                        && removeFileIfPresent(_buildIdPath)
                        && files::rename(_newPackagePath, _packagePath)
                        && files::rename(_newBuildIdPath, _buildIdPath);

        // after a failure, drop the temporary files left (errno kept for the
        // caller); after a success there are none
        if (!isInstalled) {
            int error = errno;
            removeFileIfPresent(_newPackagePath);
            removeFileIfPresent(_newBuildIdPath);
            errno = error;
        }

        return isInstalled;
    }

    /**
     * Frees the upload buffer; the installed files are not touched.
     */
    void abortUpload() override {
        memory::release(_upload);
        _upload = nullptr;
        _uploadSize = 0;
    }

    /**
     * Size of the package file and its build id (empty if app.buildid is
     * missing). False if there is no package file.
     */
    bool installed(char *buildId, size_t buildIdSize, size_t *packageSize) override {
        // the package file must exist
        files::Status status;
        if (!files::stat(_packagePath, &status) || !status.isFile) {
            return false;
        }

        *packageSize = (size_t)status.size;

        // its build id: the text of app.buildid (readAll ends it with a zero)
        size_t length = 0;
        uint8_t *text = files::readAll(_buildIdPath, &length);
        snprintf(buildId, buildIdSize, "%s", text != nullptr ? (const char *)text : "");
        memory::release(text);
        return true;
    }

private:
    char     _packagePath[1024] = "";      // <storage>/app.qjspkg
    char     _buildIdPath[1024] = "";      // <storage>/app.buildid
    char     _newPackagePath[1024] = "";   // <storage>/app.qjspkg.new, while installing
    char     _newBuildIdPath[1024] = "";   // <storage>/app.buildid.new, while installing
    uint8_t *_upload = nullptr;            // malloc'd upload buffer; nullptr: no upload open
    size_t   _uploadSize = 0;              // bytes, size of _upload
};

/**
 * The link to one connected host: the agent sends through it; its reader
 * thread (readerThread) receives, then closes the socket and deletes it.
 */
class TcpLink : public agent::Link {
public:
    /**
     * Wraps an accepted socket (closed by the reader thread, not here).
     */
    explicit TcpLink(sockets::Socket socket) : _socket(socket) {}

    /**
     * The socket of the host.
     */
    sockets::Socket socket() const {
        return _socket;
    }

    /**
     * Sends bytes to the host; blocks until all are sent. False if the
     * host is gone.
     */
    bool send(const uint8_t *data, size_t size) override {
        int error = 0;
        return sockets::sendAll(_socket, data, size, &error);
    }

private:
    sockets::Socket _socket;   // accepted socket, closed by the reader thread
};

/**
 * The TCP links: one host at a time, a new host replaces the previous one.
 * An accept thread waits for hosts; each host has a reader thread.
 */
struct Connections {
    agent::Agent    *agent;              // borrowed: the agent the links are attached to
    sockets::Socket  listener;           // listening socket of the accept thread
    platform::Mutex  mutex;              // protects current
    TcpLink         *current = nullptr;  // connected host, owned by its reader thread; nullptr: none
};

// Everything the desktop board knows (one board per process).
struct HostState {
    HostOptions  options;                  // given by configureHost()
    FileStorage  storage;                  // installed package and uploads
    Connections  connections;              // TCP links to the host
    char        *dataDirectory = nullptr;  // absolute "<storage>/data", malloc'd by configureHost()
};

HostState state;

/**
 * End of a link (its reader thread stopped, or never started): detaches it
 * from the agent if it is still the current one, then closes its socket and
 * deletes it. 'reason' is printed when it was current.
 */
void endLink(TcpLink *link, const char *reason) {
    // detach the link from the agent only if it is still the current one (a
    // replaced link was already swapped out by attach(newLink))
    state.connections.mutex.lock();
    bool stillCurrent = state.connections.current == link;
    if (stillCurrent) {
        state.connections.agent->attach(nullptr);
        state.connections.current = nullptr;
        printf("qjsx agent: %s\n", reason);
        fflush(stdout);
    }

    state.connections.mutex.unlock();

    // nobody else uses the link now: close and delete it
    sockets::closeSocket(link->socket());
    delete link;
}

/**
 * Reader thread of one host: gives the received bytes to the agent until the
 * host is gone. Owns 'argument', a TcpLink (started by acceptThread).
 */
void readerThread(void *argument) {
    TcpLink *link = (TcpLink *)argument;

    // on the heap, reused: thread stacks are small on a target
    uint8_t *buffer = (uint8_t *)memory::allocate(sockets::RECEIVE_BLOCK);
    bool reading = true;

    // give every received block to the agent until the host disconnects, the
    // socket fails, or acceptThread shuts the socket down because a new host
    // replaced this one
    while (reading) {
        int received = recv(link->socket(), (char *)buffer, (int)sockets::RECEIVE_BLOCK, 0);
        reading = received > 0;
        if (reading) {
            state.connections.agent->receive(buffer, (size_t)received);
        }
    }

    memory::release(buffer);

    // detach, close and delete the link
    endLink(link, "host disconnected");
}

/**
 * Accept thread: waits for hosts and attaches each one to the agent, forever.
 * Ends only if accept() fails.
 */
void acceptThread(void *) {
    for (;;) {
        // wait for the next host (set up with TCP_NODELAY: the small protocol
        // frames go out at once)
        int error = 0;
        sockets::Socket socket = sockets::acceptConnection(state.connections.listener, &error);
        if (socket == sockets::invalidSocket) {
            break;
        }

        // under the mutex, the new link replaces the current one
        TcpLink *link = new TcpLink(socket);
        state.connections.mutex.lock();
        if (state.connections.current != nullptr) {
            // the previous host is replaced: its reader thread ends and frees it
            sockets::shutdownSocket(state.connections.current->socket());
        }

        state.connections.current = link;
        state.connections.agent->attach(link);
        state.connections.mutex.unlock();

        // start the reader thread of the new link, which owns it from then on;
        // if it cannot start, nothing would ever read the link: end it now
        printf("qjsx agent: host connected\n");
        fflush(stdout);
        if (!platform::startThread(&readerThread, link, platform::defaultThreadOptions("agent-reader"))) {
            endLink(link, "host dropped: cannot start a reader thread");
        }
    }
}

/**
 * Absolute "<storage>/data" with '/' separators (malloc'd): the program's
 * files, like the data partition of a target. nullptr if the current
 * directory cannot be read.
 */
char *absoluteDataDirectory(const char *storage) {
    // a relative storage is relative to the current directory of the agent
    char *base = nullptr;
    if (!files::isAbsolute(storage)) {
        base = files::currentDirectory();
        if (base == nullptr) {
            return nullptr;
        }
    }

    // "<base>/<storage>/data", or "<storage>/data"
    size_t size = (base != nullptr ? strlen(base) + 1 : 0) + strlen(storage) + sizeof("/data");
    char *directory = (char *)memory::allocate(size);
    if (base != nullptr) {
        snprintf(directory, size, "%s/%s/data", base, storage);
    } else {
        snprintf(directory, size, "%s/data", storage);
    }

    memory::release(base);
    files::useSlashes(directory);
    return directory;
}

} // namespace

/**
 * Records the options and creates the storage directory and its data
 * directory (see board_host.h).
 */
bool configureHost(HostOptions options) {
    // the storage paths are known before the directory exists
    state.options = options;
    state.storage.setDirectory(options.storage);

    // create the directory (and its parents)
    if (!files::makeDirectories(options.storage)) {
        fprintf(stderr, "qjsx agent: cannot create %s: %s\n", options.storage, strerror(errno));
        return false;
    }

    // the program's files: "<storage>/data", absolute since it is process.cwd()
    memory::release(state.dataDirectory);
    state.dataDirectory = absoluteDataDirectory(options.storage);
    bool created = state.dataDirectory != nullptr && files::makeDirectories(state.dataDirectory);
    if (!created) {
        fprintf(stderr, "qjsx agent: cannot create the data directory in %s: %s\n", options.storage, strerror(errno));
        return false;
    }

    return true;
}

/**
 * "<storage>/data" (set by configureHost; nullptr before it).
 */
const char *dataDirectory() {
    return state.dataDirectory;
}

/**
 * The desktop: no flash or PSRAM size to report.
 */
void identity(Identity *identity) {
    *identity = Identity{ "desktop", "qjsx agent", 0, 0 };
}

/**
 * The desktop system, named like Node's process.platform.
 */
const char *platformName() {
#if defined(_WIN32)
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
#error "board_host.cpp: unknown desktop system, add its process.platform name"
#endif
}

/**
 * The desktop cannot restart: returns false, the runner starts the program
 * again instead.
 */
bool restart() {
    return false;   // the runner starts the program again instead
}

/**
 * The default allocator of QuickJS.
 */
const JSMallocFunctions *jsAllocator() {
    return nullptr;
}

/**
 * The desktop's threads have large stacks: QuickJS's default limit.
 */
size_t jsStackSize() {
    return 0;
}

/**
 * The file storage of <storage>/app.qjspkg.
 */
agent::Storage *packageStorage() {
    return &state.storage;
}

/**
 * Reads the whole installed package file into memory (nullptr if there is
 * none); freed by closePackage().
 */
const uint8_t *openPackage(size_t *size) {
    return files::readAll(state.storage.packagePath(), size);
}

/**
 * Frees the copy made by openPackage().
 */
void closePackage(const uint8_t *package) {
    memory::release((void *)package);
}

/**
 * The TCP link (called once by agent::runDevice): accepts hosts and attaches
 * each one to 'agent'. False if the port cannot be opened or the thread cannot start.
 */
bool startLinks(agent::Agent *agent) {
    // listen on the configured address and port (port 0: the system picks one)
    int port = 0;
    int error = 0;
    sockets::Socket listener = sockets::listenOn(state.options.host, state.options.port, &port, &error);
    if (listener == sockets::invalidSocket) {
        fprintf(stderr, "qjsx agent: cannot listen on %s:%d: %s\n", state.options.host, state.options.port,
                strerror(error));
        return false;
    }

    // what the threads need
    state.connections.agent = agent;
    state.connections.listener = listener;

    // start the accept thread
    printf("qjsx agent: listening on %s:%d, storage %s\n", state.options.host, port, state.options.storage);
    fflush(stdout);
    return platform::startThread(&acceptThread, nullptr, platform::defaultThreadOptions("agent-accept"));
}

/**
 * Console of the program when no host session is open: stdout or stderr,
 * flushed at once.
 */
void writeConsole(int stream, const char *text, size_t length) {
    FILE *out = stream == rt::OUTPUT_STDERR ? stderr : stdout;
    fwrite(text, 1, length, out);
    fflush(out);
}

} // namespace board
