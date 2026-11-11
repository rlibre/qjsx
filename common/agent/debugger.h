// Debugger stub: the device side of the debugger (portable C++, same code on
// the ESP and in the desktop agent). It answers the debugger requests of the
// agent protocol (host/js/protocol.js, kinds DEBUG_ATTACH..RELEASE) with
// the engine debugger API (engine/qjsx/quickjs-debug.h).
//
// Threads. Requests arrive on the transport thread (post()) and are queued.
// They run on the JS thread:
//   - while the program is paused (in the hook): all requests, in the pause
//     loop, until RESUME / STEP / DEBUG_DETACH;
//   - while the program runs JS code: from the interrupt handler (poll()),
//     only the requests that run no JS code ("light" ones: PAUSE, SET_STOPS,
//     SET_EXCEPTIONS, DEBUG_ATTACH, DEBUG_DETACH, LIST_SCRIPTS); the others
//     wait;
//   - while the program is idle (event loop waiting): from a loop message.
// Without a running program, DEBUG_ATTACH, DEBUG_DETACH and SET_EXCEPTIONS
// are answered at once; the other requests get an error.
//
// The debugging state (attached, exception mode) outlives a run: a program
// started again (RUN, RESET) is debugged too; the host sets its stops again
// when the modules are loaded (SCRIPT_LOADED).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-debug.h"
#include "cbor.h"
#include "loop.h"
#include "platform.h"

namespace agent {

class Agent;

class Debugger {
public:
    /**
     * Debugger of the device answered by 'agent' (not owned). Not attached,
     * no exception pause, no run.
     */
    explicit Debugger(Agent *agent);
    /**
     * Frees the queued requests and the handle table. finish() must have run
     * before (the JS values of the handles are not freed here).
     */
    ~Debugger();
    Debugger(const Debugger &) = delete;
    Debugger &operator=(const Debugger &) = delete;

    // ### transport thread ####################################################

    /**
     * A debugger request: the whole CBOR message [kind, id, payload] (copied).
     * Transport thread. Queued for the JS thread while a run exists, else
     * answered at once (see the top of this file). A message that does not
     * decode is dropped without answer, and so is a request repeated while it
     * is still queued or running (its answer comes once).
     */
    void             post(const uint8_t *message, size_t size);
    /**
     * The host is gone: debugging off, a paused program goes on. Transport
     * thread; the JS thread removes the hook and the stops at its next debug
     * event or when it next processes the queue.
     */
    void             hostGone();

    // ### any thread ##########################################################

    /**
     * A run is about to start (before start()): forgets the abort of the
     * previous run. Any thread; the runner calls it under its own lock, with
     * the run marked as running, so that abortRun() from then on is kept.
     */
    void             prepareRun();
    /**
     * The run is being stopped: leaves a pause at once (and the wait for
     * RESUME after a module load). Any thread; kept until the next
     * prepareRun().
     */
    void             abortRun();

    // ### JS thread ###########################################################

    /**
     * A run starts (loop created, nothing evaluated yet): registers the
     * debugger as a source of 'loop' and installs the hook if attached.
     */
    void             start(JSContext *ctx, rt::Loop *loop);
    /**
     * The run has ended: hook removed, handles and script list freed, the
     * requests still queued are answered with an error.
     */
    void             finish();
    /**
     * From the interrupt handler of the runtime: runs the queued light
     * requests, cheap (one atomic read) when the queue is empty.
     */
    void             poll();
    /**
     * A module was loaded and is not evaluated yet. Records its name for
     * LIST_SCRIPTS; while debugging, tells the host (SCRIPT_LOADED) and
     * waits for RESUME.
     */
    void             moduleLoaded(const char *name);

private:
    struct Request;
    struct Handle;
    struct Fields;
    enum class StepMode { None, Any, Over, Out };   // Any: step into, or a PAUSE request
    enum class Context { Running, Idle, Paused };   // state of the program when a request runs

    // ### queue ###############################################################

    /**
     * Marks the first waiting request (the first light one if 'lightOnly')
     * as taken and returns it, or nullptr. It stays in the queue (a repeat
     * of it is dropped) until finishRequest().
     */
    Request         *takeRequest(bool lightOnly);
    /**
     * A taken request has been answered: removes it from the queue and frees it.
     */
    void             finishRequest(Request *request);
    /**
     * True if a request with this id is in the queue, waiting or running
     * (_signal held).
     */
    bool             isKnownRequest(int64_t id);
    /**
     * JS thread: runs the queued requests allowed in 'context' (only the
     * light ones while JS code runs). Also removes the hook if the host left.
     */
    void             processRequests(Context context);
    /**
     * True for the requests that run no JS code and need no pause, so they
     * can run from the interrupt handler.
     */
    static bool      isLight(int64_t kind);
    /**
     * Transport thread, no run, _signal held: applies a request that only
     * changes the debugging state. Returns nullptr if applied, else the
     * error to answer (static text).
     */
    const char      *applyWithoutRuntime(int64_t kind, const Fields *fields);
    /**
     * JS thread: true while the host is attached. When it has left
     * (hostGone), first removes the hook and every stop.
     */
    bool             followHost();
    /**
     * Loop callback (JS thread, program idle): runs the queued requests.
     */
    static void      onLoopMessage(void *opaque, const rt::Message *message);

    // ### requests (JS thread) ################################################

    /**
     * Decodes 'request' and calls the handler of its kind. A request that
     * does not decode is dropped without answer.
     */
    void             handle(Request *request, Context context);
    /**
     * Answers DEBUG_ATTACH: debugging on, hook installed.
     */
    void             handleAttach(int64_t id);
    /**
     * Answers DEBUG_DETACH: debugging off, hook and stops removed, a pause
     * is left.
     */
    void             handleDetach(int64_t id);
    /**
     * Answers PAUSE: the program stops at the next statement start (no-op
     * if already paused).
     */
    void             handlePause(int64_t id);
    /**
     * Answers RESUME: leaves the pause (or the wait after a module load);
     * no effect outside of them.
     */
    void             handleResume(int64_t id, Context context);
    /**
     * Answers STEP { STEP: into / over / out }: starts the step and leaves
     * the pause. Error if not paused.
     */
    void             handleStep(int64_t id, const Fields *fields, Context context);
    /**
     * Answers SET_STOPS { MODULE, STOP_KIND, DATA } with { MISSING }: the
     * number of functions of DATA not found in the module.
     */
    void             handleSetStops(int64_t id, const Fields *fields);
    /**
     * Answers SET_EXCEPTIONS { MODE }: none, uncaught or all.
     */
    void             handleSetExceptions(int64_t id, const Fields *fields);
    /**
     * Answers LIST_SCRIPTS with { SCRIPTS }: the modules loaded by the run.
     */
    void             handleListScripts(int64_t id);
    /**
     * Answers GET_STACK with { FRAMES }, innermost frame first. Paused only.
     */
    void             handleGetStack(int64_t id, Context context);
    /**
     * Answers GET_SLOTS { LEVEL, SLOT_KIND, GROUP? } with { VALUES }: every
     * slot of that kind in that frame. Paused only.
     */
    void             handleGetSlots(int64_t id, const Fields *fields, Context context);
    /**
     * Answers SET_SLOT { LEVEL, SLOT_KIND, INDEX, VALUE }. Paused only.
     */
    void             handleSetSlot(int64_t id, const Fields *fields, Context context);
    /**
     * Answers GET_PROPERTIES { HANDLE, GROUP? } with { PROPERTIES, INTERNALS }
     * of the object. Runs no getter.
     */
    void             handleGetProperties(int64_t id, const Fields *fields);
    /**
     * Answers LOAD_FUNCTION { DATA, GROUP? } with { VALUE }: evaluates the
     * compiled bytecode sent by the host, or { EXCEPTION } if it throws.
     */
    void             handleLoadFunction(int64_t id, const Fields *fields);
    /**
     * Answers CALL { FUNCTION, THIS?, ARGUMENTS?, GROUP? } with { VALUE }, or
     * { EXCEPTION } if the call throws. Never pauses inside the call.
     */
    void             handleCall(int64_t id, const Fields *fields);
    /**
     * Answers RELEASE { GROUP } or { HANDLE }: frees handles.
     */
    void             handleRelease(int64_t id, const Fields *fields);

    // ### hook and pause ######################################################

    /**
     * Debug hook given to the engine: forwards to onHook().
     */
    static int       hook(JSContext *ctx, const JSDebugEvent *event, void *opaque);
    /**
     * JS thread, inside the interpreter: decides whether the event pauses
     * the program, and pauses it. Always returns 0 (the program goes on).
     */
    int              onHook(const JSDebugEvent *event);
    /**
     * At a statement start while stepping: true if the current step ends here.
     */
    bool             shouldStopStepping();
    /**
     * Pauses the program inside the hook: PAUSED event, requests until the
     * program goes on, RESUMED event. 'reason' is a protocol REASON_*.
     */
    void             pause(int reason, const JSDebugEvent *event);
    /**
     * Runs the requests (Paused context) until RESUME / STEP / DEBUG_DETACH,
     * the host leaves or the run is stopped.
     */
    void             waitInPauseLoop();
    /**
     * Installs the debug hook and the exception reporting of the current mode.
     */
    void             installHook();
    /**
     * Removes the hook, stepping and exception reporting. Stops are kept.
     */
    void             removeHook();
    /**
     * Starts a step of 'mode' from the current pause (or a pause request).
     */
    void             startStep(StepMode mode);
    /**
     * Number of frames of the stack (only valid inside the hook).
     */
    int              stackDepth();

    // ### values (debugger_values.cpp) ########################################

    /**
     * Writes 'value' in the VALUE encoding of the protocol; objects and
     * symbols get a handle in 'group'.
     */
    void             writeValue(cbor::Writer *writer, JSValueConst value, int group);
    /**
     * Writes an object value: handle in 'group', kind, class name,
     * description, size and function location.
     */
    void             writeObject(cbor::Writer *writer, JSValueConst object, int group);
    /**
     * Writes the own properties of 'object' (at most MAX_PROPERTIES) and adds
     * their number to *propertyCount. The caller writes the array header.
     */
    void             writeProperties(cbor::Writer *writer, JSValueConst object, int group, int *propertyCount);
    /**
     * Writes the internal properties of 'object' ([[Prototype]]...) and sets
     * *internalCount. The caller writes the array header.
     */
    void             writeInternals(cbor::Writer *writer, JSValueConst object, int group, int *internalCount);
    /**
     * Reads a value sent by the host into *value (new reference). False if
     * it is invalid or names an unknown handle.
     */
    bool             readValue(cbor::Reader *reader, JSValue *value);
    /**
     * Short description of an object, like Chrome's ("Array(3)"). malloc'd.
     */
    char            *describeObject(JSValueConst object, const JSDebugObjectInfo *info, const char *className);
    /**
     * Name shown for the class of an object (constructor name). malloc'd.
     */
    char            *objectClassName(JSValueConst object, const JSDebugObjectInfo *info);
    /**
     * The "length" of an array, read without running JS code, or -1.
     */
    int64_t          arrayLength(JSValueConst object);

    // ### handles #############################################################

    /**
     * Keeps a reference on 'value' in 'group'; returns its new handle.
     */
    int64_t          addHandle(JSValueConst value, int group);
    /**
     * New reference on the value of 'handle' in *value; false if unknown.
     */
    bool             findHandle(int64_t handle, JSValue *value);
    /**
     * Frees every handle of 'group'.
     */
    void             releaseGroup(int group);
    /**
     * Frees one handle (no-op if unknown).
     */
    void             releaseHandle(int64_t handle);
    /**
     * Frees every handle (end of a run).
     */
    void             releaseAllHandles();

    // ### answers #############################################################

    /**
     * Answers request 'id' with a payload map of 'fieldCount' fields.
     */
    void             respond(int64_t id, const cbor::Writer *payload, int fieldCount);
    /**
     * Answers request 'id' with an empty payload.
     */
    void             respondEmpty(int64_t id);
    /**
     * Answers request 'id' with an error message.
     */
    void             respondError(int64_t id, const char *message);
    /**
     * Sends an event of 'kind' with a payload map of 'fieldCount' fields.
     */
    void             sendEvent(int kind, const cbor::Writer *payload, int fieldCount);
    /**
     * Answers request 'id' with { EXCEPTION }: the pending JS exception
     * (cleared), its objects in 'group'.
     */
    void             respondException(int64_t id, int group);

    Agent           *_agent;  // borrowed

    // shared with the transport thread (_signal held)
    platform::Signal _signal;         // lock of these fields; wakes the pause loop
    Request         *_firstRequest;   // queue head (waiting and taken requests), owned; nullptr: empty
    Request         *_lastRequest;    // queue tail; nullptr: empty
    int              _pendingCount;   // waiting (not taken) requests; also read without the lock by poll()
    bool             _attached;       // debugging on (DEBUG_ATTACH received)
    bool             _runAborted;     // the run is being stopped: leave the pause
    int              _exceptionMode;  // protocol EXCEPTIONS_*
    rt::Mailbox     *_mailbox;        // retained while a run exists
    rt::SourceId     _loopSource;     // our source in the loop of the run
    bool             _runActive;      // a run exists: requests are queued

    // JS thread only
    JSContext       *_ctx;              // context of the run; nullptr: no run
    rt::Loop        *_loop;             // loop of the run; nullptr: no run
    bool             _hookInstalled;    // debug hook set in the engine
    bool             _paused;           // in the pause loop (program stopped in the hook)
    bool             _resumeRequested;  // RESUME / STEP received: leave the pause loop
    int              _requestDepth;     // > 0 while a request runs JS code: no pause then
    StepMode         _stepMode;         // step in progress; None: not stepping
    uintptr_t        _stepFrameId;      // frame where the step started
    int              _stepDepth;        // its depth (number of frames below + 1)
    Handle          *_handles;          // malloc'd array; its values are owned
    int              _handleCount;      // used entries of _handles
    int              _handleCapacity;   // allocated entries of _handles
    int64_t          _nextHandle;       // id of the next handle (from 1; 0 is never used)
    char           **_scripts;          // modules loaded by this run
    int              _scriptCount;      // entries of _scripts
};

} // namespace agent
