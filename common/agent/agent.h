// Device agent: the part of the firmware that talks with the host tool
// (deploy, run, stop, console; the debugger requests go to the debugger
// stub, common/agent/debugger.h). Platform
// independent: the platform gives a Link (serial or TCP), a Storage (flash
// partition on the ESP, files on the desktop) and a Controller (starts and
// stops the JS program).
//
// Threads: receive() is called by the transport thread; sendLog(),
// sendExited(), sendStarted() may be called from any thread.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cbor.h"
#include "frames.h"
#include "platform.h"
#include "sha256.h"

namespace agent {

class Debugger;

// Connection to the host (serial port or TCP socket), given by the platform.
class Link {
public:
    /**
     * Virtual destructor, for the platform implementations.
     */
    virtual      ~Link() {}

    /**
     * Sends whole frames to the host, with the agent mutex held (frames never interleave).
     * False on a write error, which the agent ignores: the transport notices the broken link.
     */
    virtual bool send(const uint8_t *data, size_t size) = 0;
};

// Where packages are stored (flash partition on the ESP, files on the
// desktop). Called on the transport thread only, one upload at a time.
class Storage {
public:
    /**
     * Virtual destructor, for the platform implementations.
     */
    virtual      ~Storage() {}

    /**
     * Prepares the reception of a package of 'size' bytes (erases the partition, opens a
     * temporary file...). Returns false if it cannot hold it.
     */
    virtual bool beginUpload(size_t size) = 0;

    /**
     * Writes a chunk at 'offset'; chunks come in order, without gaps.
     */
    virtual bool writeUpload(size_t offset, const uint8_t *data, size_t size) = 0;

    /**
     * Installs the uploaded package with its build id (all bytes received,
     * SHA-256 checked). Ends the upload, whether it succeeds or not.
     */
    virtual bool finishUpload(const char *buildId) = 0;

    /**
     * Drops the upload in progress; the installed package stays.
     */
    virtual void abortUpload() = 0;

    /**
     * Build id (null terminated, at most 'buildIdSize' bytes) and size of
     * the installed package. Returns false if none.
     */
    virtual bool installed(char *buildId, size_t buildIdSize, size_t *packageSize) = 0;
};

// Starts and stops the JS program, given by the platform.
class Controller {
public:
    /**
     * Virtual destructor, for the platform implementations.
     */
    virtual      ~Controller() {}

    /**
     * Stops the running program if any, starts the installed package.
     */
    virtual void run() = 0;

    /**
     * Stops the running program (nothing if none).
     */
    virtual void stop() = 0;

    /**
     * Restarts the device (ESP: restarts the chip).
     */
    virtual void reset() = 0;

    /**
     * True while a program runs.
     */
    virtual bool running() = 0;
};

struct Identity {
    const char *name;       // "espjs desktop agent", "esp32-s3"...
    const char *engine;     // QuickJS version
    const char *token;      // required in HELLO if not null (network links)
    int         bytecode;   // version of the bytecode the engine reads (JS_GetBytecodeVersion)
};

// The agent itself: decodes the frames received from the host, answers the
// requests (HELLO, INFO, upload, RUN/STOP/RESET), forwards the debugger
// requests to the Debugger and sends answers and events back.
class Agent {
public:
    /**
     * The agent borrows 'storage' and 'controller' (not owned); the strings
     * of 'identity' must outlive it. No host is attached yet.
     */
    Agent(Storage *storage, Controller *controller, Identity identity);

    /**
     * Frees the upload hash and the remembered last answers.
     */
    ~Agent();

    Agent(const Agent &) = delete;
    Agent &operator=(const Agent &) = delete;

    /**
     * A host connected (link, borrowed until the next attach()) or disconnected (nullptr).
     * Either way the session closes (HELLO needed again) and a debugger session ends.
     */
    void            attach(Link *link);

    /**
     * Bytes received from the host, any cut (transport thread). Complete messages are handled
     * before it returns, except the debugger requests: queued for the JS thread.
     */
    void            receive(const uint8_t *data, size_t size);

    /**
     * True while a host session is open (HELLO done): the console output
     * of the program goes to the host as LOG events.
     */
    bool            sessionOpen();

    /**
     * Sends console output as a LOG event (any thread). Returns false if
     * there is no session (the caller prints locally).
     */
    bool            sendLog(int stream, const char *text, size_t length);

    /**
     * EXITED event: the program ended with 'code' (any thread; nothing sent
     * without a session).
     */
    void            sendExited(int code);

    /**
     * STARTED event: the package 'buildId' started (any thread; nothing sent
     * without a session).
     */
    void            sendStarted(const char *buildId);

    /**
     * The debugger requests (DEBUG_ATTACH..RELEASE) go to 'debugger' (may
     * be null: they are refused). Set before the host can connect.
     */
    void            setDebugger(Debugger *debugger) {
        _debugger = debugger;
    }

    /**
     * RESPONSE of the debugger to its request 'id', from any thread. 'payload' holds
     * 'fieldCount' encoded key/value pairs. Kept as the debugger's last answer.
     */
    void            answerDebugRequest(int64_t id, const cbor::Writer *payload, int fieldCount);

    /**
     * ERROR answer of the debugger to its request 'id', with a text message, from any thread.
     * Kept as the debugger's last answer.
     */
    void            answerDebugError(int64_t id, const char *message);

    /**
     * If 'id' is the request of the last answer of the agent or of the debugger, sends that
     * answer again (the host did not get it) and returns true. Any thread.
     */
    bool            resendAnswer(int64_t id);

    /**
     * Event 'kind' (id 0), from any thread; only sent during a session.
     * 'payload' holds 'fieldCount' encoded key/value pairs.
     */
    void            sendEvent(int kind, const cbor::Writer *payload, int fieldCount);

private:
    struct Fields;

    // The last answer of one answerer (the agent itself or the debugger): a repeated request
    // gets it again. Each answerer has its own, so that the answers of one never replace
    // those of the other.
    struct LastAnswer {
        int64_t  requestId;  // -1: none
        uint8_t *frame;      // malloc'd frame, as sent; nullptr: none
        size_t   size;       // bytes of 'frame'
    };

    /**
     * Parser callback: one decoded message, on the transport thread.
     */
    static void     onMessage(void *opaque, const uint8_t *message, size_t size);

    /**
     * Reads the payload of a request (map or null) into 'fields'. Returns
     * false if it is malformed.
     */
    static bool     readFields(cbor::Reader *reader, Fields *fields);

    /**
     * Dispatches an agent request (not a debugger one) to its handler.
     */
    void            handle(int64_t kind, int64_t id, const Fields *fields);

    /**
     * HELLO: checks protocol version and token, opens the session.
     */
    void            handleHello(int64_t id, const Fields *fields);

    /**
     * INFO: running state, installed build id and package size.
     */
    void            handleInfo(int64_t id);

    /**
     * UPLOAD_BEGIN: starts receiving a package (size, SHA-256, build id).
     */
    void            handleUploadBegin(int64_t id, const Fields *fields);

    /**
     * UPLOAD_CHUNK: writes the next chunk of the package.
     */
    void            handleUploadChunk(int64_t id, const Fields *fields);

    /**
     * UPLOAD_END: checks the SHA-256 and installs the package.
     */
    void            handleUploadEnd(int64_t id);

    /**
     * RESPONSE of the agent to request 'id'. 'payload' holds 'fieldCount' encoded pairs.
     */
    void            respond(int64_t id, const cbor::Writer *payload, int fieldCount);

    /**
     * RESPONSE of the agent to request 'id' with an empty payload map.
     */
    void            respondEmpty(int64_t id);

    /**
     * ERROR answer of the agent to request 'id', with a text message.
     */
    void            respondError(int64_t id, const char *message);

    /**
     * Sends [ERROR, id, {MESSAGE}], kept in 'answer'.
     */
    void            sendError(LastAnswer *answer, int64_t id, const char *message);

    /**
     * Sends message [kind, id, {payload}]: header + map head + payload pairs. An answer is
     * kept in 'answer' (nullptr for an event).
     */
    void            sendWithPayload(int kind, int64_t id, const cbor::Writer *payload, int fieldCount,
                                    LastAnswer *answer);

    /**
     * Frames and sends an encoded message. An answer to request 'requestId' is kept in
     * 'answer' (replacing the one there), to be sent again if the host repeats the request;
     * 'answer' is nullptr for an event.
     */
    void            sendMessage(const uint8_t *message, size_t size, int64_t requestId, LastAnswer *answer);

    /**
     * Frees the frame of 'answer': no last answer any more (_mutex held, or no other thread).
     */
    static void     forgetAnswer(LastAnswer *answer);

    Storage        *_storage;     // borrowed
    Controller     *_controller;  // borrowed
    Debugger       *_debugger;    // borrowed; nullptr: debugger requests refused
    Identity        _identity;    // its strings are borrowed
    frames::Parser  _parser;      // frames from the host (transport thread)
    platform::Mutex _mutex;       // _link, _session, last answers, writes
    Link           *_link;        // borrowed; nullptr: no host (_mutex held)
    bool            _session;     // HELLO done (_mutex held)

    // package upload (transport thread only)
    bool            _uploading;          // between UPLOAD_BEGIN and UPLOAD_END
    size_t          _uploadSize;         // bytes announced by UPLOAD_BEGIN
    size_t          _uploadReceived;     // bytes written so far
    uint8_t         _uploadHash[32];     // expected SHA-256 of the package
    char            _uploadBuildId[64];  // null terminated
    crypto::Sha256 *_hash;               // owned; hash of the bytes received; nullptr: no upload yet

    // last answers: a repeated request gets the same answer (_mutex held)
    LastAnswer      _agentAnswer;     // of the requests answered by the agent itself
    LastAnswer      _debuggerAnswer;  // of the debugger requests
};

} // namespace agent
