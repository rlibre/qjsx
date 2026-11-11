#include "debugger.h"
#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "protocol.h"

namespace agent {

/**
 * A queued request: the whole message.
 */
struct Debugger::Request {
    uint8_t *data;    // malloc'd copy of the message
    size_t size;      // bytes of 'data'
    int64_t kind;     // protocol request kind
    int64_t id;       // request id, for the answer
    bool taken;       // running on the JS thread: stays queued until answered
    Request *next;    // next in the queue; nullptr: last
};

/**
 * An object kept for the host, known by its handle.
 */
struct Debugger::Handle {
    int64_t id;      // handle sent to the host
    JSValue value;   // owned
    int group;       // protocol GROUP_*, released together
};

/**
 * Payload of a request: for each key, where its value is in the message.
 * Values are decoded on demand (they may be nested: arrays, JS values).
 */
struct Debugger::Fields {
    struct Field {
        int64_t key;           // protocol KEY_*
        const uint8_t *data;   // encoded value, borrowed from the message
        size_t size;           // bytes of the encoded value
    };
    Field items[16];   // the first 'count' are used
    int count = 0;     // fields indexed

    /**
     * Indexes the payload (an integer-keyed map, or null for no fields).
     * False if the payload is not a valid map.
     */
    bool parse(cbor::Reader *reader) {
        // a null payload has no fields
        if (reader->peek() == cbor::Type::Null) {
            return reader->null();
        }

        // otherwise a map
        uint64_t mapCount = 0;
        if (!reader->map(&mapCount)) {
            return false;
        }

        // for each entry
        for (uint64_t i = 0; i < mapCount; i++) {
            // read the key
            Field item = { 0, nullptr, 0 };
            if (!reader->integer(&item.key)) {
                return false;
            }

            // skip the value, without decoding it
            size_t start = reader->position();
            if (!reader->skip()) {
                return false;
            }

            // record where the value's bytes are (they stay in the message);
            // entries beyond the 16 slots are dropped silently
            item.data = reader->data() + start;
            item.size = reader->position() - start;
            int capacity = (int)(sizeof(items) / sizeof(items[0]));
            if (count < capacity) {
                items[count++] = item;
            }
        }

        return true;
    }

    /**
     * Reader on the value of 'key' (false if the key is missing).
     */
    bool value(int64_t key, cbor::Reader *reader) const {
        // linear search: 16 fields at most
        for (int i = 0; i < count; i++) {
            if (items[i].key == key) {
                *reader = cbor::Reader(items[i].data, items[i].size);
                return true;
            }
        }

        return false;
    }

    /**
     * Integer value of 'key' (false if missing or not an integer).
     */
    bool integer(int64_t key, int64_t *result) const {
        cbor::Reader reader(nullptr, 0);
        return value(key, &reader) && reader.integer(result);
    }

    /**
     * Byte string value of 'key', pointing into the message (false if missing
     * or not bytes).
     */
    bool bytes(int64_t key, const uint8_t **data, size_t *size) const {
        cbor::Reader reader(nullptr, 0);
        return value(key, &reader) && reader.bytes(data, size);
    }

    /**
     * Text value of 'key', pointing into the message, not zero-terminated
     * (false if missing or not text).
     */
    bool text(int64_t key, const char **text, size_t *length) const {
        cbor::Reader reader(nullptr, 0);
        return value(key, &reader) && reader.text(text, length);
    }
};

namespace {

// Loop message kind of the debugger source: requests are waiting.
const int LOOP_MESSAGE_REQUESTS = 1;
// The pause loop checks this often whether the run was stopped.
const int PAUSE_WAIT_MS = 50;

// most arguments of a CALL (js-debug helpers take a few; the count comes from
// the wire and sizes an allocation)
const uint64_t MAX_CALL_ARGUMENTS = 256;

// bytes of the header of an entry of SET_STOPS DATA: u16 function number and
// u32 offset count
const size_t STOP_ENTRY_HEADER = 6;

// error answered to a SET_EXCEPTIONS without a valid MODE
const char *const SET_EXCEPTIONS_ERROR = "SET_EXCEPTIONS: MODE (0 none, 1 uncaught, 2 all) expected";

/**
 * Reads the header [kind, id, payload] of a message; the reader is left on
 * the payload.
 */
bool readHeader(cbor::Reader *reader, int64_t *kind, int64_t *id) {
    uint64_t count = 0;
    return reader->array(&count) && count == 3 && reader->integer(kind) && reader->integer(id);
}

/**
 * Big-endian 32-bit integer at 'data'.
 */
uint32_t readU32(const uint8_t *data) {
    return (uint32_t)(data[0]) << 24 | (uint32_t)(data[1]) << 16
         | (uint32_t)(data[2]) << 8 | data[3];
}

/**
 * Big-endian 16-bit integer at 'data'.
 */
uint16_t readU16(const uint8_t *data) {
    return (uint16_t)(data[0] << 8 | data[1]);
}

/**
 * Reads the header of the SET_STOPS entry at *position in DATA (function
 * number, offset count) and moves *position to its offsets. False if the
 * entry, offsets included, does not fit in DATA.
 */
bool readStopEntry(const uint8_t *data, size_t size, size_t *position, int *number, uint32_t *count) {
    // the header must fit (*position never passes 'size')
    bool headerFits = size - *position >= STOP_ENTRY_HEADER;
    if (!headerFits) {
        return false;
    }

    // read it
    *number = readU16(data + *position);
    *count = readU32(data + *position + 2);
    *position += STOP_ENTRY_HEADER;

    // the offsets must fit too: 'count' comes from the wire, it is compared
    // with what remains, never multiplied before (no overflow)
    size_t remaining = size - *position;
    return *count <= remaining / 4;
}

/**
 * True if SET_STOPS DATA is a sequence of whole entries (none truncated).
 */
bool isStopsDataValid(const uint8_t *data, size_t size) {
    size_t position = 0;

    while (position < size) {
        // the entry header, then skip its offsets
        int number = 0;
        uint32_t count = 0;
        if (!readStopEntry(data, size, &position, &number, &count)) {
            return false;
        }

        position += (size_t)count * 4;
    }

    return true;
}

/**
 * True for a protocol EXCEPTIONS_* mode: none, uncaught or all.
 */
bool isExceptionMode(int64_t mode) {
    return mode >= EXCEPTIONS_NONE && mode <= EXCEPTIONS_ALL;
}

/**
 * Engine slot kind of a protocol SLOT_* kind (unknown kinds: locals).
 */
JSDebugSlotKind engineSlotKind(int64_t kind) {
    switch (kind) {
        case SLOT_ARGUMENT: {
            return JS_DEBUG_SLOT_ARGUMENT;
        }
        case SLOT_CLOSURE: {
            return JS_DEBUG_SLOT_CLOSURE;
        }
        default: {
            return JS_DEBUG_SLOT_LOCAL;
        }
    }
}

/**
 * Number of slots of a protocol SLOT_* kind in 'frame' (unknown kinds: locals).
 */
int slotCount(const JSDebugFrame *frame, int64_t kind) {
    switch (kind) {
        case SLOT_ARGUMENT: {
            return frame->argument_count;
        }
        case SLOT_CLOSURE: {
            return frame->closure_count;
        }
        default: {
            return frame->local_count;
        }
    }
}

} // namespace

/**
 * Starts detached, without run, with an empty queue and no handle.
 */
Debugger::Debugger(Agent *agent)
    : _agent(agent), _firstRequest(nullptr), _lastRequest(nullptr), _pendingCount(0), _attached(false),
      _runAborted(false), _exceptionMode(EXCEPTIONS_NONE), _mailbox(nullptr), _loopSource{ -1, 0 },
      _runActive(false), _ctx(nullptr), _loop(nullptr), _hookInstalled(false), _paused(false),
      _resumeRequested(false), _requestDepth(0), _stepMode(StepMode::None), _stepFrameId(0), _stepDepth(0),
      _handles(nullptr), _handleCount(0), _handleCapacity(0), _nextHandle(1), _scripts(nullptr), _scriptCount(0) {
}

/**
 * Frees the requests left in the queue (unanswered) and the handle array.
 */
Debugger::~Debugger() {
    // the requests left in the queue: freed without answer
    while (_firstRequest != nullptr) {
        Request *request = _firstRequest;
        _firstRequest = request->next;
        memory::release(request->data);
        memory::release(request);
    }

    // the handle array (its values were freed by finish())
    memory::release(_handles);
}

// ### Transport thread ########################################################

/**
 * The light kinds: they change the debugging state or read what is known
 * without a pause, and run no JS code.
 */
bool Debugger::isLight(int64_t kind) {
    switch (kind) {
        case KIND_DEBUG_ATTACH:
        case KIND_DEBUG_DETACH:
        case KIND_PAUSE:
        case KIND_SET_STOPS:
        case KIND_SET_EXCEPTIONS:
        case KIND_LIST_SCRIPTS: {
            return true;
        }
        default: {
            return false;
        }
    }
}

/**
 * A debugger request (transport thread): queued for the JS thread while a
 * run exists, else answered at once.
 */
void Debugger::post(const uint8_t *message, size_t size) {
    // decode the header and index the payload; a message that does not
    // decode is dropped without answer
    cbor::Reader reader(message, size);
    int64_t kind = 0;
    int64_t id = 0;
    Fields requestFields;
    if (!readHeader(&reader, &kind, &id) || !requestFields.parse(&reader)) {
        return;
    }

    // copy the message into a new Request: the caller's buffer does not
    // outlive this call
    Request *request = (Request *)memory::allocate(sizeof(Request));
    request->data = (uint8_t *)memory::allocate(size);
    memcpy(request->data, message, size);
    request->size = size;
    request->kind = kind;
    request->id = id;
    request->taken = false;
    request->next = nullptr;

    // under _signal, a repeated request is not run twice. The agent tested
    // its last answers before this call, but the JS thread may have answered
    // since (it answers before finishRequest() takes _signal): test them
    // again here, then the queue (still waiting or running: dropped, its
    // answer will come)
    _signal.lock();
    bool repeated = _agent->resendAnswer(id) || isKnownRequest(id);

    // a new request while a run exists: append it to the queue and count it
    bool queued = !repeated && _runActive;
    if (queued) {
        if (_lastRequest != nullptr) {
            _lastRequest->next = request;
        } else {
            _firstRequest = request;
        }

        _lastRequest = request;
        __atomic_add_fetch(&_pendingCount, 1, __ATOMIC_RELEASE);

        // then wake the event loop if it is idle (loop message) and the
        // pause loop (signal)
        _mailbox->post(_loopSource, LOOP_MESSAGE_REQUESTS, nullptr, 0, 0);
        _signal.notify();
    }

    // a new request without run: the debugging state changes now, in the
    // same critical section as the test of _runActive, so that a run
    // starting meanwhile (start()) sees the change
    bool appliedNow = !repeated && !queued;
    const char *error = nullptr;
    if (appliedNow) {
        error = applyWithoutRuntime(kind, &requestFields);
    }

    _signal.unlock();

    // answer a request applied now, outside the lock
    if (appliedNow && error != nullptr) {
        respondError(id, error);
    } else if (appliedNow) {
        respondEmpty(id);
    }

    // not queued: free the copy
    if (!queued) {
        memory::release(request->data);
        memory::release(request);
    }
}

/**
 * No program is running (transport thread, _signal held): only the
 * debugging state can change; the next start() reads it (and installs the
 * hook if attached).
 */
const char *Debugger::applyWithoutRuntime(int64_t kind, const Fields *fields) {
    switch (kind) {
        case KIND_DEBUG_ATTACH: {
            _attached = true;
            return nullptr;
        }
        case KIND_DEBUG_DETACH: {
            _attached = false;
            return nullptr;
        }
        case KIND_SET_EXCEPTIONS: {
            // MODE: none, uncaught or all
            int64_t mode = 0;
            bool valid = fields->integer(KEY_MODE, &mode) && isExceptionMode(mode);
            if (!valid) {
                return SET_EXCEPTIONS_ERROR;
            }

            _exceptionMode = (int)mode;
            return nullptr;
        }
        default: {
            // every other request needs a program
            return "no program running";
        }
    }
}

/**
 * The host is gone (transport thread): debugging off; the JS thread sees it
 * in followHost() (next debug event, processRequests(), moduleLoaded()) and
 * in waitInPauseLoop().
 */
void Debugger::hostGone() {
    // debugging off (under _signal: the JS thread reads it)
    _signal.lock();
    _attached = false;

    // wake the idle event loop, if a run exists: the JS thread removes the
    // hook when it sees the change
    if (_mailbox != nullptr) {
        _mailbox->post(_loopSource, LOOP_MESSAGE_REQUESTS, nullptr, 0, 0);
    }

    // and wake the pause loop: a paused program goes on
    _signal.notify();
    _signal.unlock();
}

/**
 * A run is about to start (any thread, before start()): the abort of the
 * previous run is forgotten.
 */
void Debugger::prepareRun() {
    _signal.lock();
    _runAborted = false;
    _signal.unlock();
}

/**
 * The run is being stopped: sets _runAborted and wakes the pause loop, which
 * then returns. Reset by the next prepareRun() (not by start(): a stop that
 * arrives while the run is starting must be kept).
 */
void Debugger::abortRun() {
    _signal.lock();
    _runAborted = true;
    _signal.notify();
    _signal.unlock();
}

// ### JS thread: run start and end, request processing ########################

/**
 * A run starts (JS thread, before any evaluation): requests are queued from
 * now on, the hook is installed if the host is attached.
 */
void Debugger::start(JSContext *ctx, rt::Loop *loop) {
    // keep the context and the loop; the debugger becomes a source of the loop
    _ctx = ctx;
    _loop = loop;
    _loopSource = loop->addSource(&onLoopMessage, this);

    // under _signal: retain the mailbox (post() uses it from the transport
    // thread) and mark the run active: requests are queued from now on; a
    // DEBUG_ATTACH applied without run before this point is seen here
    _signal.lock();
    _mailbox = loop->mailbox();
    _mailbox->retain();
    _runActive = true;
    bool attached = _attached;
    _signal.unlock();

    // attached: install the hook now, so the first statements of the
    // program can already stop
    if (attached) {
        installHook();
    }
}

/**
 * The run has ended (JS thread, context still alive): frees what belongs to
 * the run and answers the requests still queued with an error.
 */
void Debugger::finish() {
    // remove the hook; free the handles now (their values belong to the context)
    removeHook();
    releaseAllHandles();

    // free the list of loaded modules
    for (int i = 0; i < _scriptCount; i++) {
        memory::release(_scripts[i]);
    }

    memory::release(_scripts);
    _scripts = nullptr;
    _scriptCount = 0;

    // under _signal: the run is no longer active (new requests are answered
    // without runtime); take the queue, release the mailbox
    _signal.lock();
    _runActive = false;
    Request *orphans = _firstRequest;
    _firstRequest = nullptr;
    _lastRequest = nullptr;
    __atomic_store_n(&_pendingCount, 0, __ATOMIC_RELEASE);
    _mailbox->release();
    _mailbox = nullptr;
    _signal.unlock();

    // outside the lock: answer the orphan requests with an error
    while (orphans != nullptr) {
        Request *request = orphans;
        orphans = request->next;
        respondError(request->id, "the program has ended");
        memory::release(request->data);
        memory::release(request);
    }

    // leave the loop, forget the context and the loop
    _loop->removeSource(_loopSource);
    _loop = nullptr;
    _ctx = nullptr;
}

/**
 * From the interrupt handler (JS thread, JS code running): runs the queued
 * light requests.
 */
void Debugger::poll() {
    // cheap check first, without the lock: nothing queued, nothing to do
    if (__atomic_load_n(&_pendingCount, __ATOMIC_ACQUIRE) == 0) {
        return;
    }

    // JS code is running: only the light requests
    processRequests(Context::Running);
}

/**
 * Loop message posted by post() or hostGone(): the program is idle, every
 * request can run.
 */
void Debugger::onLoopMessage(void *opaque, const rt::Message *) {
    ((Debugger *)opaque)->processRequests(Context::Idle);
}

/**
 * Marks the first waiting request (the first light one if 'lightOnly') as
 * taken and returns it, or nullptr. It stays queued until finishRequest().
 */
Debugger::Request *Debugger::takeRequest(bool lightOnly) {
    // under _signal: find the first request not taken yet, skipping the heavy
    // ones if 'lightOnly' (a light request may thus run before heavier ones
    // queued earlier; a taken one is running further up the JS stack)
    _signal.lock();
    Request *request = _firstRequest;

    for (; request != nullptr; request = request->next) {
        bool allowed = !lightOnly || isLight(request->kind);
        if (!request->taken && allowed) {
            break;
        }
    }

    // found: mark it taken and count it out of the waiting requests
    if (request != nullptr) {
        request->taken = true;
        __atomic_sub_fetch(&_pendingCount, 1, __ATOMIC_RELEASE);
    }

    _signal.unlock();
    return request;
}

/**
 * A taken request has been answered (JS thread): removes it from the queue
 * and frees it.
 */
void Debugger::finishRequest(Request *request) {
    // under _signal: find the request before it, unlink it (fixing
    // _lastRequest if it was the last)
    _signal.lock();
    Request *previous = nullptr;

    for (Request *entry = _firstRequest; entry != request; entry = entry->next) {
        previous = entry;
    }

    if (previous != nullptr) {
        previous->next = request->next;
    } else {
        _firstRequest = request->next;
    }

    if (_lastRequest == request) {
        _lastRequest = previous;
    }

    _signal.unlock();

    // outside the lock: free it
    memory::release(request->data);
    memory::release(request);
}

/**
 * True if a request with this id is queued, waiting or taken (_signal held).
 */
bool Debugger::isKnownRequest(int64_t id) {
    for (Request *request = _firstRequest; request != nullptr; request = request->next) {
        if (request->id == id) {
            return true;
        }
    }

    return false;
}

/**
 * JS thread: true while the host is attached. When it has left (hostGone)
 * with the hook installed, debugging stops here: hook and every stop removed.
 */
bool Debugger::followHost() {
    // read the attached flag (the transport thread writes it)
    _signal.lock();
    bool attached = _attached;
    _signal.unlock();

    // the host went away while the hook is installed: debugging off
    bool hostLeft = !attached && _hookInstalled;
    if (hostLeft) {
        removeHook();
        JS_DebugClearAllStops(_ctx);
    }

    return attached;
}

/**
 * Runs the queued requests allowed in 'context' (JS thread). Also removes
 * the hook if the host left.
 */
void Debugger::processRequests(Context context) {
    // the host went away (hostGone): debugging off first
    followHost();

    // while JS code runs, only the light requests
    bool lightOnly = context == Context::Running;

    // take the requests one by one, handle each, then remove it from the
    // queue; the lock is not held while a request runs
    for (;;) {
        Request *request = takeRequest(lightOnly);
        if (request == nullptr) {
            break;
        }

        handle(request, context);
        finishRequest(request);
    }
}

/**
 * Decodes a queued request and calls the handler of its kind. A request
 * that does not decode is dropped without answer.
 */
void Debugger::handle(Request *request, Context context) {
    // decode the header and the payload fields again (post() kept only kind
    // and id)
    cbor::Reader reader(request->data, request->size);
    int64_t kind = 0;
    int64_t id = 0;
    Fields requestFields;
    if (!readHeader(&reader, &kind, &id) || !requestFields.parse(&reader)) {
        return;
    }

    // dispatch on the kind; unknown kinds get an error
    switch (kind) {
        case KIND_DEBUG_ATTACH: {
            handleAttach(id);
            break;
        }
        case KIND_DEBUG_DETACH: {
            handleDetach(id);
            break;
        }
        case KIND_PAUSE: {
            handlePause(id);
            break;
        }
        case KIND_RESUME: {
            handleResume(id, context);
            break;
        }
        case KIND_STEP: {
            handleStep(id, &requestFields, context);
            break;
        }
        case KIND_SET_STOPS: {
            handleSetStops(id, &requestFields);
            break;
        }
        case KIND_SET_EXCEPTIONS: {
            handleSetExceptions(id, &requestFields);
            break;
        }
        case KIND_LIST_SCRIPTS: {
            handleListScripts(id);
            break;
        }
        case KIND_GET_STACK: {
            handleGetStack(id, context);
            break;
        }
        case KIND_GET_SLOTS: {
            handleGetSlots(id, &requestFields, context);
            break;
        }
        case KIND_SET_SLOT: {
            handleSetSlot(id, &requestFields, context);
            break;
        }
        case KIND_GET_PROPERTIES: {
            handleGetProperties(id, &requestFields);
            break;
        }
        case KIND_LOAD_FUNCTION: {
            handleLoadFunction(id, &requestFields);
            break;
        }
        case KIND_CALL: {
            handleCall(id, &requestFields);
            break;
        }
        case KIND_RELEASE: {
            handleRelease(id, &requestFields);
            break;
        }
        default: {
            respondError(id, "unknown debugger request");
            break;
        }
    }
}

// ### Requests ################################################################

/**
 * Answers DEBUG_ATTACH: debugging on, hook installed.
 */
void Debugger::handleAttach(int64_t id) {
    // debugging on (under _signal: the transport thread reads it too)
    _signal.lock();
    _attached = true;
    _signal.unlock();

    // install the hook now: the program can stop from here on
    installHook();
    respondEmpty(id);
}

/**
 * Answers DEBUG_DETACH: debugging off, hook and stops removed, a pause (or
 * the wait after a module load) is left.
 */
void Debugger::handleDetach(int64_t id) {
    // debugging off (under _signal: the transport thread reads it too)
    _signal.lock();
    _attached = false;
    _signal.unlock();

    // remove the hook and every stop, then leave a pause, if any
    removeHook();
    JS_DebugClearAllStops(_ctx);
    _resumeRequested = true;
    respondEmpty(id);
}

/**
 * Answers PAUSE: the program stops at the next statement start. Already
 * paused: nothing to do.
 */
void Debugger::handlePause(int64_t id) {
    // a pause request is a step "into": any statement start stops
    if (!_paused) {
        startStep(StepMode::Any);
    }

    respondEmpty(id);
}

/**
 * Answers RESUME: leaves a pause (or the wait after a module load); no
 * effect elsewhere.
 */
void Debugger::handleResume(int64_t id, Context context) {
    // really paused: cancel any step
    if (context == Context::Paused && _paused) {
        _stepMode = StepMode::None;
        JS_DebugSetStepping(JS_GetRuntime(_ctx), false);
    }

    // in the pause loop (pause or wait after a module load): ask it to
    // return. In the wait after a module load, a step in progress or a PAUSE
    // received during the wait is kept: the program stops at its next
    // statement
    if (context == Context::Paused) {
        _resumeRequested = true;
    }

    respondEmpty(id);
}

/**
 * Answers STEP: starts a step into, over or out and leaves the pause.
 */
void Debugger::handleStep(int64_t id, const Fields *fields, Context context) {
    // the step kind: into, over or out
    int64_t kind = 0;
    if (!fields->integer(KEY_STEP, &kind) || kind < STEP_INTO || kind > STEP_OUT) {
        respondError(id, "STEP: STEP (1 into, 2 over, 3 out) expected");
        return;
    }

    // the program must be really paused (not only waiting after a module load)
    if (context != Context::Paused || !_paused) {
        respondError(id, "STEP: the program is not paused");
        return;
    }

    // start the step from the frame of the pause and ask the pause loop to
    // return; the hook ends the step (shouldStopStepping)
    StepMode mode = kind == STEP_INTO ? StepMode::Any : kind == STEP_OVER ? StepMode::Over : StepMode::Out;
    startStep(mode);
    _resumeRequested = true;
    respondEmpty(id);
}

/**
 * Answers SET_STOPS: replaces the stops of the functions listed in DATA,
 * answers { MISSING }, the number of functions not found.
 */
void Debugger::handleSetStops(int64_t id, const Fields *fields) {
    // MODULE, STOP_KIND and DATA
    const char *moduleText = nullptr;
    size_t moduleLength = 0;
    int64_t stopKind = 0;
    const uint8_t *data = nullptr;
    size_t size = 0;
    bool valid = fields->text(KEY_MODULE, &moduleText, &moduleLength) && fields->integer(KEY_STOP_KIND, &stopKind)
              && fields->bytes(KEY_DATA, &data, &size);
    if (!valid) {
        respondError(id, "SET_STOPS: MODULE, STOP_KIND and DATA expected");
        return;
    }

    // DATA holds, for each function: a big-endian u16 function number, a u32
    // count and 'count' u32 bytecode offsets (host/js/protocol.js,
    // encodeStops); it is checked whole first, so that a truncated DATA
    // changes no stop
    if (!isStopsDataValid(data, size)) {
        respondError(id, "SET_STOPS: truncated DATA");
        return;
    }

    // the module name is copied zero-terminated for the engine
    JSDebugStopKind engineKind = stopKind == STOPS_BREAKPOINTS ? JS_DEBUG_STOPS_BREAKPOINTS : JS_DEBUG_STOPS_STATEMENTS;
    char *module = memory::copyText(moduleText, moduleLength);
    int missing = 0;
    size_t position = 0;

    while (position < size) {
        // the entry header (DATA was checked: the entry fits)
        int number = 0;
        uint32_t count = 0;
        readStopEntry(data, size, &position, &number, &count);

        // decode the offsets into a temporary array
        uint32_t *offsets = (uint32_t *)memory::allocate(sizeof(uint32_t) * ((size_t)count + 1));

        for (uint32_t i = 0; i < count; i++) {
            offsets[i] = readU32(data + position + i * 4);
        }

        position += (size_t)count * 4;

        // replace the stops of that function; a function not found counts
        // as missing
        if (JS_DebugSetStops(_ctx, module, number, engineKind, offsets, (int)count) < 0) {
            missing++;
        }

        memory::release(offsets);
    }

    memory::release(module);

    // answer { MISSING }
    cbor::Writer payload;
    payload.integer(KEY_MISSING);
    payload.integer(missing);
    respond(id, &payload, 1);
}

/**
 * Answers SET_EXCEPTIONS: stores the mode (none, uncaught or all) and
 * applies it now if the hook is installed.
 */
void Debugger::handleSetExceptions(int64_t id, const Fields *fields) {
    // MODE: none, uncaught or all
    int64_t mode = 0;
    bool valid = fields->integer(KEY_MODE, &mode) && isExceptionMode(mode);
    if (!valid) {
        respondError(id, SET_EXCEPTIONS_ERROR);
        return;
    }

    // store it under _signal: the transport thread reads it too
    _signal.lock();
    _exceptionMode = (int)mode;
    _signal.unlock();

    // hook installed: turn the exception reporting of the engine on or off
    // now (else installHook() will)
    if (_hookInstalled) {
        JS_DebugSetExceptionReporting(JS_GetRuntime(_ctx), mode != EXCEPTIONS_NONE);
    }

    respondEmpty(id);
}

/**
 * Answers LIST_SCRIPTS with { SCRIPTS: [module name...] }, in load order.
 */
void Debugger::handleListScripts(int64_t id) {
    cbor::Writer payload;
    payload.integer(KEY_SCRIPTS);
    payload.array(_scriptCount);

    for (int i = 0; i < _scriptCount; i++) {
        payload.text(_scripts[i]);
    }

    respond(id, &payload, 1);
}

/**
 * Answers GET_STACK with { FRAMES }, innermost frame first. Paused only.
 */
void Debugger::handleGetStack(int64_t id, Context context) {
    // paused only
    if (context != Context::Paused || !_paused) {
        respondError(id, "GET_STACK: the program is not paused");
        return;
    }

    // one entry per frame, level 0 (innermost) first, into a separate writer
    // (the count is known only at the end): [frame id, module name or null
    // (native frame), function number, pc, argument count, local count,
    // closure count]
    cbor::Writer frames;
    int count = 0;
    JSDebugFrame frame;

    while (JS_DebugGetFrame(_ctx, count, &frame) == 0) {
        frames.array(7);
        frames.integer((int64_t)frame.id);
        if (frame.is_bytecode && frame.module_name != JS_ATOM_NULL) {
            const char *module = JS_AtomToCString(_ctx, frame.module_name);
            frames.text(module != nullptr ? module : "");
            JS_FreeCString(_ctx, module);
        } else {
            frames.null();
        }

        frames.integer(frame.function_number);
        frames.integer(frame.pc);
        frames.integer(frame.argument_count);
        frames.integer(frame.local_count);
        frames.integer(frame.closure_count);
        count++;
    }

    // answer { FRAMES }: the array header, then those entries
    cbor::Writer payload;
    payload.integer(KEY_FRAMES);
    payload.array(count);
    payload.encoded(frames.data(), frames.size());
    respond(id, &payload, 1);
}

/**
 * Answers GET_SLOTS with { VALUES }: every slot of one kind in one frame.
 * Paused only.
 */
void Debugger::handleGetSlots(int64_t id, const Fields *fields, Context context) {
    // LEVEL and SLOT_KIND
    int64_t level = 0;
    int64_t kind = 0;
    int64_t group = GROUP_PAUSE;
    if (!fields->integer(KEY_LEVEL, &level) || !fields->integer(KEY_SLOT_KIND, &kind)) {
        respondError(id, "GET_SLOTS: LEVEL and SLOT_KIND expected");
        return;
    }

    // the optional GROUP of the handles (default: pause)
    fields->integer(KEY_GROUP, &group);

    // the frame, paused only
    JSDebugFrame frame;
    bool found = context == Context::Paused && _paused && JS_DebugGetFrame(_ctx, (int)level, &frame) == 0;
    if (!found) {
        respondError(id, "GET_SLOTS: no such frame (or not paused)");
        return;
    }

    // write every slot of that kind as a value
    int count = slotCount(&frame, kind);
    cbor::Writer payload;
    payload.integer(KEY_VALUES);
    payload.array(count);

    for (int i = 0; i < count; i++) {
        // a slot the engine cannot give (closure variable not created yet)
        // is written as undefined
        JSValue value = JS_DebugGetSlot(_ctx, (int)level, engineSlotKind(kind), i);
        if (JS_IsException(value)) {
            payload.undefined();
            continue;
        }

        writeValue(&payload, value, (int)group);
        JS_FreeValue(_ctx, value);
    }

    respond(id, &payload, 1);
}

/**
 * Answers SET_SLOT: writes VALUE in a slot of a frame. Paused only.
 */
void Debugger::handleSetSlot(int64_t id, const Fields *fields, Context context) {
    // LEVEL, SLOT_KIND, INDEX and VALUE
    int64_t level = 0;
    int64_t kind = 0;
    int64_t index = 0;
    cbor::Reader valueReader(nullptr, 0);
    bool valid = fields->integer(KEY_LEVEL, &level) && fields->integer(KEY_SLOT_KIND, &kind)
              && fields->integer(KEY_INDEX, &index) && fields->value(KEY_VALUE, &valueReader);
    if (!valid) {
        respondError(id, "SET_SLOT: LEVEL, SLOT_KIND, INDEX and VALUE expected");
        return;
    }

    // paused only
    if (context != Context::Paused || !_paused) {
        respondError(id, "SET_SLOT: the program is not paused");
        return;
    }

    // decode VALUE (a handle gives a new reference on its object)
    JSValue value = JS_UNDEFINED;
    if (!readValue(&valueReader, &value)) {
        respondError(id, "SET_SLOT: invalid VALUE");
        return;
    }

    // give it to the engine, which takes it in every case
    if (JS_DebugSetSlot(_ctx, (int)level, engineSlotKind(kind), (int)index, value) < 0) {
        respondError(id, "SET_SLOT: no such frame or slot");
        return;
    }

    respondEmpty(id);
}

/**
 * Answers GET_PROPERTIES with { PROPERTIES, INTERNALS } of the object of
 * HANDLE. Runs no getter.
 */
void Debugger::handleGetProperties(int64_t id, const Fields *fields) {
    // HANDLE
    int64_t handle = 0;
    int64_t group = GROUP_PAUSE;
    if (!fields->integer(KEY_HANDLE, &handle)) {
        respondError(id, "GET_PROPERTIES: HANDLE expected");
        return;
    }

    // the optional GROUP of the handles (default: pause)
    fields->integer(KEY_GROUP, &group);

    // the object (new reference)
    JSValue object = JS_UNDEFINED;
    if (!findHandle(handle, &object)) {
        respondError(id, "GET_PROPERTIES: unknown handle");
        return;
    }

    // its own properties into a separate writer (the count is known only at
    // the end)
    cbor::Writer properties;
    int propertyCount = 0;
    writeProperties(&properties, object, (int)group, &propertyCount);

    // its internals into another one; the object is no longer needed
    cbor::Writer internals;
    int internalCount = 0;
    writeInternals(&internals, object, (int)group, &internalCount);
    JS_FreeValue(_ctx, object);

    // answer { PROPERTIES, INTERNALS }: each array header followed by its
    // entries
    cbor::Writer payload;
    payload.integer(KEY_PROPERTIES);
    payload.array(propertyCount);
    payload.encoded(properties.data(), properties.size());
    payload.integer(KEY_INTERNALS);
    payload.array(internalCount);
    payload.encoded(internals.data(), internals.size());
    respond(id, &payload, 2);
}

/**
 * Answers LOAD_FUNCTION: evaluates bytecode compiled by the host (the
 * helpers of js-debug), answers { VALUE } or { EXCEPTION }.
 */
void Debugger::handleLoadFunction(int64_t id, const Fields *fields) {
    // DATA
    const uint8_t *data = nullptr;
    size_t size = 0;
    int64_t group = GROUP_PERSISTENT;
    if (!fields->bytes(KEY_DATA, &data, &size)) {
        respondError(id, "LOAD_FUNCTION: DATA expected");
        return;
    }

    // the optional GROUP of the handles (default: persistent)
    fields->integer(KEY_GROUP, &group);

    // read the bytecode and evaluate it with _requestDepth raised, so that
    // no stop pauses meanwhile; the result is the loaded value
    _requestDepth++;
    JSValue script = JS_ReadObject(_ctx, data, size, JS_READ_OBJ_BYTECODE);
    JSValue function = JS_IsException(script) ? JS_EXCEPTION : JS_EvalFunction(_ctx, script);
    _requestDepth--;

    // reading or evaluating threw: answer { EXCEPTION }
    if (JS_IsException(function)) {
        respondException(id, (int)group);
        return;
    }

    // else answer { VALUE }, a handle in GROUP
    cbor::Writer payload;
    payload.integer(KEY_VALUE);
    writeValue(&payload, function, (int)group);
    JS_FreeValue(_ctx, function);
    respond(id, &payload, 1);
}

/**
 * Answers CALL: calls FUNCTION with THIS and ARGUMENTS, answers { VALUE } or
 * { EXCEPTION }. Never pauses inside the call.
 */
void Debugger::handleCall(int64_t id, const Fields *fields) {
    // the optional GROUP of the handles (default: pause)
    int64_t group = GROUP_PAUSE;
    fields->integer(KEY_GROUP, &group);

    // FUNCTION is required; THIS and ARGUMENTS are optional
    cbor::Reader functionReader(nullptr, 0);
    cbor::Reader thisReader(nullptr, 0);
    cbor::Reader argumentsReader(nullptr, 0);
    if (!fields->value(KEY_FUNCTION, &functionReader)) {
        respondError(id, "CALL: FUNCTION expected");
        return;
    }

    // decode FUNCTION (new reference, freed at the end)
    JSValue function = JS_UNDEFINED;
    JSValue thisValue = JS_UNDEFINED;
    if (!readValue(&functionReader, &function)) {
        respondError(id, "CALL: invalid FUNCTION");
        return;
    }

    // decode THIS if given (new reference, freed at the end)
    if (fields->value(KEY_THIS, &thisReader) && !readValue(&thisReader, &thisValue)) {
        JS_FreeValue(_ctx, function);
        respondError(id, "CALL: invalid THIS");
        return;
    }

    // ARGUMENTS: an array, else no arguments
    uint64_t argumentCount = 0;
    bool hasArguments = fields->value(KEY_ARGUMENTS, &argumentsReader) && argumentsReader.array(&argumentCount);
    if (!hasArguments) {
        argumentCount = 0;
    }

    // the count comes from the wire: bounded before it sizes an allocation
    if (argumentCount > MAX_CALL_ARGUMENTS) {
        JS_FreeValue(_ctx, function);
        JS_FreeValue(_ctx, thisValue);
        respondError(id, "CALL: too many ARGUMENTS");
        return;
    }

    // decode them into a malloc'd array, up to the first invalid one
    JSValue *arguments = (JSValue *)memory::allocate(sizeof(JSValue) * ((size_t)argumentCount + 1));

    uint64_t readCount = 0;
    bool argumentsValid = true;

    while (readCount < argumentCount && argumentsValid) {
        argumentsValid = readValue(&argumentsReader, &arguments[readCount]);
        if (argumentsValid) {
            readCount++;
        }
    }

    // all valid: call the function with _requestDepth raised (no stop pauses
    // inside the call), answer { VALUE } or { EXCEPTION }; else an error
    if (argumentsValid) {
        _requestDepth++;
        JSValue result = JS_Call(_ctx, function, thisValue, (int)readCount, arguments);
        _requestDepth--;

        if (JS_IsException(result)) {
            respondException(id, (int)group);
        } else {
            cbor::Writer payload;
            payload.integer(KEY_VALUE);
            writeValue(&payload, result, (int)group);
            respond(id, &payload, 1);
        }

        JS_FreeValue(_ctx, result);
    } else {
        respondError(id, "CALL: invalid ARGUMENTS");
    }

    // free the arguments read, the array, the function and 'this'
    for (uint64_t i = 0; i < readCount; i++) {
        JS_FreeValue(_ctx, arguments[i]);
    }

    memory::release(arguments);
    JS_FreeValue(_ctx, function);
    JS_FreeValue(_ctx, thisValue);
}

/**
 * Answers RELEASE: frees the handles of GROUP if given, else the handle
 * HANDLE.
 */
void Debugger::handleRelease(int64_t id, const Fields *fields) {
    int64_t group = 0;
    int64_t handle = 0;
    if (fields->integer(KEY_GROUP, &group)) {
        releaseGroup((int)group);
    } else if (fields->integer(KEY_HANDLE, &handle)) {
        releaseHandle(handle);
    } else {
        respondError(id, "RELEASE: GROUP or HANDLE expected");
        return;
    }

    respondEmpty(id);
}

// ### Hook, steps and pause ###################################################

/**
 * Installs the debug hook and the exception reporting of the current mode.
 */
void Debugger::installHook() {
    // the exception mode (under _signal: the transport thread writes it)
    JSRuntime *rt = JS_GetRuntime(_ctx);
    _signal.lock();
    int mode = _exceptionMode;
    _signal.unlock();

    // install the hook; report exceptions unless the mode is "none"
    JS_SetDebugHook(rt, &hook, this);
    JS_DebugSetExceptionReporting(rt, mode != EXCEPTIONS_NONE);
    _hookInstalled = true;
}

/**
 * Removes the hook, stepping and exception reporting; a step in progress is
 * cancelled, stops are kept. No-op without a run.
 */
void Debugger::removeHook() {
    // no run: nothing is installed
    if (_ctx == nullptr) {
        return;
    }

    // engine side: no hook, no stepping, no exception reporting; our side:
    // no step in progress
    JSRuntime *rt = JS_GetRuntime(_ctx);
    JS_SetDebugHook(rt, nullptr, nullptr);
    JS_DebugSetStepping(rt, false);
    JS_DebugSetExceptionReporting(rt, false);
    _stepMode = StepMode::None;
    _hookInstalled = false;
}

/**
 * Number of frames from the frame of the hook down to the outermost one.
 */
int Debugger::stackDepth() {
    JSDebugFrame frame;
    int depth = 0;

    while (JS_DebugGetFrame(_ctx, depth, &frame) == 0) {
        depth++;
    }

    return depth;
}

/**
 * Starts a step of 'mode' from the current pause (its frame is level 0), or
 * for a pause request (StepMode::Any: no frame needed).
 */
void Debugger::startStep(StepMode mode) {
    // record the mode, no start frame yet
    _stepMode = mode;
    _stepFrameId = 0;
    _stepDepth = 0;

    // over and out: remember the frame where the step starts, by id and depth
    JSDebugFrame frame;
    if (mode != StepMode::Any && JS_DebugGetFrame(_ctx, 0, &frame) == 0) {
        _stepFrameId = frame.id;
        _stepDepth = stackDepth();
    }

    // stepping on: the hook is called at every statement start
    JS_DebugSetStepping(JS_GetRuntime(_ctx), true);
}

/**
 * A statement start was reached while stepping: true if the current step
 * ends here.
 */
bool Debugger::shouldStopStepping() {
    JSDebugFrame frame;

    switch (_stepMode) {
        case StepMode::Any: {
            // into: any statement
            return true;
        }
        case StepMode::Over: {
            // over: a statement of the same frame (also after an 'await': the
            // frame of an async function keeps its id)
            if (JS_DebugGetFrame(_ctx, 0, &frame) == 0 && frame.id == _stepFrameId) {
                return true;
            }

            // or of a calling frame: the stack is shallower than at the start
            return stackDepth() < _stepDepth;
        }
        case StepMode::Out: {
            // out: a statement of a calling frame (shallower stack)
            return stackDepth() < _stepDepth;
        }
        case StepMode::None: {
            break;
        }
    }

    return false;
}

/**
 * Debug hook given to the engine: forwards to onHook() of the debugger
 * passed as 'opaque'.
 */
int Debugger::hook(JSContext *, const JSDebugEvent *event, void *opaque) {
    return ((Debugger *)opaque)->onHook(event);
}

/**
 * Decides whether a debug event pauses the program, and pauses it (JS
 * thread, inside the interpreter). Returns 0: the program goes on.
 */
int Debugger::onHook(const JSDebugEvent *event) {
    // code run for the host (LOAD_FUNCTION, CALL, accessors run to describe
    // an object) never stops
    if (_requestDepth > 0) {
        return 0;
    }

    // the host left (hostGone) while the program kept running: debugging
    // stops here (hook and stops removed), never a pause for a host gone
    if (!followHost()) {
        return 0;
    }

    // the exception mode (the transport thread writes it too)
    _signal.lock();
    int mode = _exceptionMode;
    _signal.unlock();

    // pause on a breakpoint, a 'debugger;' statement, the end of a step or a
    // thrown value
    switch (event->reason) {
        case JS_DEBUG_REASON_BREAKPOINT: {
            pause(REASON_BREAKPOINT, event);
            break;
        }
        case JS_DEBUG_REASON_STEP: {
            if (shouldStopStepping()) {
                pause(REASON_STEP, event);
            }

            break;
        }
        case JS_DEBUG_REASON_STATEMENT: {
            pause(REASON_DEBUGGER, event);
            break;
        }
        case JS_DEBUG_REASON_EXCEPTION: {
            // "all" pauses at the throw; "uncaught" needs the prediction of the
            // catch: not done yet, it never pauses
            if (mode == EXCEPTIONS_ALL) {
                pause(REASON_EXCEPTION, event);
            }

            break;
        }
    }

    // the program goes on (and an exception keeps unwinding)
    return 0;
}

/**
 * Pauses the program inside the hook: PAUSED event, requests until the
 * program goes on, RESUMED event.
 */
void Debugger::pause(int reason, const JSDebugEvent *event) {
    // end any step (stepping off); paused from now on
    _stepMode = StepMode::None;
    JS_DebugSetStepping(JS_GetRuntime(_ctx), false);
    _paused = true;
    _resumeRequested = false;

    // build PAUSED { REASON, EXCEPTION?, RETURN_VALUE? }: the thrown value at
    // an exception stop, as a handle of the pause group
    cbor::Writer payload;
    int fieldCount = 1;
    payload.integer(KEY_REASON);
    payload.integer(reason);
    if (event->reason == JS_DEBUG_REASON_EXCEPTION) {
        payload.integer(KEY_EXCEPTION);
        writeValue(&payload, event->exception, GROUP_PAUSE);
        fieldCount++;
    }

    // the return value at a return stop, as a handle of the pause group
    if (event->at_return) {
        payload.integer(KEY_RETURN_VALUE);
        writeValue(&payload, event->return_value, GROUP_PAUSE);
        fieldCount++;
    }

    // tell the host
    sendEvent(KIND_PAUSED, &payload, fieldCount);

    // run the requests until the program goes on; a STEP there turns
    // stepping on again
    waitInPauseLoop();

    // the pause is over: free its handles, tell the host
    _paused = false;
    releaseGroup(GROUP_PAUSE);
    cbor::Writer empty;
    sendEvent(KIND_RESUMED, &empty, 0);
}

/**
 * Runs the requests (Paused context) until RESUME / STEP / DEBUG_DETACH, the
 * host leaves or the run is stopped.
 */
void Debugger::waitInPauseLoop() {
    for (;;) {
        // run every queued request; return if one of them asked to go on
        // (RESUME, STEP, DEBUG_DETACH)
        processRequests(Context::Paused);
        if (_resumeRequested) {
            _resumeRequested = false;
            return;
        }

        // under _signal: leave if the host left or the run is aborted; with
        // an empty queue, wait for the signal (at most PAUSE_WAIT_MS) and
        // check again
        _signal.lock();
        bool leave = !_attached || _runAborted;
        bool noneWaiting = __atomic_load_n(&_pendingCount, __ATOMIC_ACQUIRE) == 0;
        if (!leave && noneWaiting) {
            _signal.wait(PAUSE_WAIT_MS);
            leave = !_attached || _runAborted;
        }

        _signal.unlock();

        if (leave) {
            return;
        }
    }
}

/**
 * A module was loaded, not evaluated yet (JS thread): records its name;
 * while debugging, tells the host and waits for its RESUME.
 */
void Debugger::moduleLoaded(const char *name) {
    // append a copy of its name to the list for LIST_SCRIPTS
    _scripts = (char **)memory::reallocate(_scripts, sizeof(char *) * (_scriptCount + 1));
    _scripts[_scriptCount++] = memory::copyText(name, strlen(name));

    // not debugging (or the host left: hook removed now): the evaluation
    // starts at once
    followHost();
    if (!_hookInstalled) {
        return;
    }

    // tell the host: SCRIPT_LOADED { MODULE }
    cbor::Writer payload;
    payload.integer(KEY_MODULE);
    payload.text(name);
    sendEvent(KIND_SCRIPT_LOADED, &payload, 1);

    // wait in the pause loop (not a pause: _paused stays false) while the
    // host sets the stops of the module; its RESUME lets the evaluation start
    _resumeRequested = false;
    waitInPauseLoop();
}

// ### Handles #################################################################

/**
 * Keeps a new reference on 'value' in 'group'; returns its new handle. Handle numbers are
 * never reused.
 */
int64_t Debugger::addHandle(JSValueConst value, int group) {
    // full: grow the array (32, then doubling)
    if (_handleCount == _handleCapacity) {
        int capacity = _handleCapacity == 0 ? 32 : _handleCapacity * 2;
        _handles = (Handle *)memory::reallocate(_handles, sizeof(Handle) * capacity);
        _handleCapacity = capacity;
    }

    // append the value with the next handle number
    Handle &handle = _handles[_handleCount++];
    handle.id = _nextHandle++;
    handle.value = JS_DupValue(_ctx, value);
    handle.group = group;
    return handle.id;
}

/**
 * New reference on the value of 'handle' (linear search); false if unknown.
 */
bool Debugger::findHandle(int64_t handle, JSValue *value) {
    for (int i = 0; i < _handleCount; i++) {
        if (_handles[i].id == handle) {
            *value = JS_DupValue(_ctx, _handles[i].value);
            return true;
        }
    }

    return false;
}

/**
 * Frees the handles of 'group'; the other handles keep their order.
 */
void Debugger::releaseGroup(int group) {
    // free the values of the group, move the others down in place
    int kept = 0;

    for (int i = 0; i < _handleCount; i++) {
        if (_handles[i].group == group) {
            JS_FreeValue(_ctx, _handles[i].value);
        } else {
            _handles[kept++] = _handles[i];
        }
    }

    _handleCount = kept;
}

/**
 * Frees one handle (no-op if unknown); the others keep their order.
 */
void Debugger::releaseHandle(int64_t handle) {
    // free its value, move the others down in place
    int kept = 0;

    for (int i = 0; i < _handleCount; i++) {
        if (_handles[i].id == handle) {
            JS_FreeValue(_ctx, _handles[i].value);
        } else {
            _handles[kept++] = _handles[i];
        }
    }

    _handleCount = kept;
}

/**
 * Frees every handle (end of a run); the array itself is kept for the next
 * run.
 */
void Debugger::releaseAllHandles() {
    for (int i = 0; i < _handleCount; i++) {
        JS_FreeValue(_ctx, _handles[i].value);
    }

    _handleCount = 0;
}

// ### Answers #################################################################

/**
 * Sends the answer through the agent (kept as the debugger's last answer).
 */
void Debugger::respond(int64_t id, const cbor::Writer *payload, int fieldCount) {
    _agent->answerDebugRequest(id, payload, fieldCount);
}

/**
 * Sends the empty answer through the agent.
 */
void Debugger::respondEmpty(int64_t id) {
    cbor::Writer payload;
    respond(id, &payload, 0);
}

/**
 * Sends the error answer through the agent (kept as the debugger's last
 * answer).
 */
void Debugger::respondError(int64_t id, const char *message) {
    _agent->answerDebugError(id, message);
}

/**
 * Sends the event through the agent.
 */
void Debugger::sendEvent(int kind, const cbor::Writer *payload, int fieldCount) {
    _agent->sendEvent(kind, payload, fieldCount);
}

/**
 * Answers { EXCEPTION: value } with the pending exception (taken, so it is
 * cleared from the context), its objects in 'group'.
 */
void Debugger::respondException(int64_t id, int group) {
    JSValue exception = JS_GetException(_ctx);
    cbor::Writer payload;
    payload.integer(KEY_EXCEPTION);
    writeValue(&payload, exception, group);
    JS_FreeValue(_ctx, exception);
    respond(id, &payload, 1);
}

} // namespace agent
