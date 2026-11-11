#include "loop.h"
#include "memory.h"

#include <stdlib.h>

namespace rt {

namespace {
// Loop of the JS thread (Loop::current()).
thread_local Loop *currentLoop = nullptr;
// Timer being run, so that clearInterval() from its own callback works.
thread_local int runningTimerId = 0;
thread_local bool runningTimerCleared = false;
}

// ### Mailbox #################################################################

/**
 * Empty, open, one reference (the loop's).
 */
Mailbox::Mailbox() : _first(nullptr), _last(nullptr), _closed(false), _references(1) {}

/**
 * Frees the queued messages. Reached through release() only, with the last
 * reference: no other thread can touch the queue any more, so no lock.
 */
Mailbox::~Mailbox() {
    dropAll();
}

/**
 * Adds a reference; atomic, so no lock.
 */
void Mailbox::retain() {
    platform::atomicIncrement(&_references);
}

/**
 * Drops a reference; the thread that drops the last one (the loop or a
 * producer, whichever ends last) deletes the mailbox.
 */
void Mailbox::release() {
    if (platform::atomicDecrement(&_references) == 0) {
        delete this;
    }
}

/**
 * Frees the queued messages and their data. The caller holds the lock
 * (close) or the last reference (destructor).
 */
void Mailbox::dropAll() {
    // unlink each message from the head, free it with its data
    while (_first != nullptr) {
        Message *message = _first;
        _first = message->next;
        memory::release(message->data);
        memory::release(message);
    }

    _last = nullptr;
}

/**
 * Queues a message for the JS thread and wakes it. Any thread.
 */
void Mailbox::post(SourceId source, int kind, uint8_t *data, size_t size, int64_t value) {
    // build the message outside the lock: malloc may be slow
    Message *message = (Message *)memory::allocate(sizeof(Message));
    *message = Message{ source, kind, data, size, value, nullptr };

    // under the lock: a closed mailbox (loop gone) frees the message and its
    // data instead of queueing them
    _signal.lock();
    if (_closed) {
        _signal.unlock();
        memory::release(data);
        memory::release(message);
        return;
    }

    // append it at the end of the list, then wake the JS thread
    if (_last != nullptr) {
        _last->next = message;
    } else {
        _first = message;
    }

    _last = message;
    _signal.notify();
    _signal.unlock();
}

/**
 * Unlinks the first message, waiting for one at most timeoutMs. JS thread;
 * the caller owns the returned message.
 */
Message *Mailbox::take(int64_t timeoutMs) {
    // under the lock, wait once if the queue is empty (timeoutMs 0: never wait)
    _signal.lock();
    if (_first == nullptr && timeoutMs != 0) {
        _signal.wait(timeoutMs);   // the caller loops: early wake ups are harmless
    }

    // unlink the first message, if any (nullptr after a timeout or an early
    // wake up)
    Message *message = _first;
    if (message != nullptr) {
        _first = message->next;
        if (_first == nullptr) {
            _last = nullptr;
        }
    }

    _signal.unlock();
    return message;
}

/**
 * True when no message is queued (reads the head under the lock).
 */
bool Mailbox::empty() {
    _signal.lock();
    bool isEmpty = _first == nullptr;
    _signal.unlock();
    return isEmpty;
}

/**
 * Closes the mailbox: post() then frees what it gets. Frees the queued
 * messages.
 */
void Mailbox::close() {
    // under the lock: refuse the future messages, drop the queued ones
    _signal.lock();
    _closed = true;
    dropAll();
    _signal.unlock();
}

// ### Loop ####################################################################

struct Loop::Timer {
    int id;            // > 0, returned by addTimer()
    int64_t deadline;  // ms, platform::monotonicMs()
    int64_t interval;  // < 0: not repeating
    JSValue callback;  // owned
    int argc;          // number of values in argv
    JSValue *argv;     // malloc'd; values owned
    Timer *next;       // next by deadline; nullptr: last
};

struct Loop::Rejection {
    JSValue promise;  // owned
    JSValue reason;   // owned
    Rejection *next;  // older rejection; nullptr: last
};

/**
 * Creates the mailbox, becomes the current loop of the thread and installs
 * the promise rejection tracker.
 */
Loop::Loop(JSContext *ctx)
    : _ctx(ctx), _mailbox(new Mailbox()), _sources(nullptr), _sourceCount(0), _firstTimer(nullptr),
      _nextTimerId(1), _firstRejection(nullptr), _holds(0), _shutDown(false), _exitRequested(false),
      _exitCode(0), _exitOnUncaught(true) {
    // become the current loop of the thread; the tracker points to this
    // loop, shutdown() removes it
    currentLoop = this;
    JS_SetHostPromiseRejectionTracker(JS_GetRuntime(ctx), &rejectionTracker, this);
}

/**
 * Frees the JS values held by the loop (timers, rejections). Before
 * JS_FreeContext; a second call does nothing.
 */
void Loop::shutdown() {
    // once only
    if (_shutDown) {
        return;
    }

    _shutDown = true;

    // free the pending timers (callbacks and arguments)
    while (_firstTimer != nullptr) {
        Timer *timer = _firstTimer;
        _firstTimer = timer->next;
        freeTimer(timer);
    }

    // free the unreported rejections (promise and reason)
    while (_firstRejection != nullptr) {
        Rejection *rejection = _firstRejection;
        _firstRejection = rejection->next;
        JS_FreeValue(_ctx, rejection->promise);
        JS_FreeValue(_ctx, rejection->reason);
        memory::release(rejection);
    }

    // remove the rejection tracker: it points to this loop
    JS_SetHostPromiseRejectionTracker(JS_GetRuntime(_ctx), nullptr, nullptr);
}

/**
 * Closes the mailbox and frees the sources. No JS here: the runtime may
 * already be freed (see shutdown()).
 */
Loop::~Loop() {
    // close the mailbox (producers still holding it post into the void), and
    // drop the loop's reference (the last producer frees it otherwise)
    _mailbox->close();
    _mailbox->release();

    // free the source table, stop being the current loop
    memory::release(_sources);
    if (currentLoop == this) {
        currentLoop = nullptr;
    }
}

/**
 * The loop of the calling JS thread (set by the constructor and run()).
 */
Loop *Loop::current() {
    return currentLoop;
}

// ### sources #################################################################

/**
 * Registers a message handler in a free slot of the source table, which
 * grows when full.
 */
SourceId Loop::addSource(MessageHandler *handler, void *opaque) {
    // find a free slot
    int index = 0;

    while (index < _sourceCount && _sources[index].used) {
        index++;
    }

    // none free: double the table (8 slots first); the new slots are free,
    // with generation 0
    if (index == _sourceCount) {
        int newCount = _sourceCount == 0 ? 8 : _sourceCount * 2;
        Source *grown = (Source *)memory::reallocate(_sources, newCount * sizeof(Source));

        for (int i = _sourceCount; i < newCount; i++) {
            grown[i] = Source{ nullptr, nullptr, 0, false };
        }

        _sources = grown;
        _sourceCount = newCount;
    }

    // fill the slot; its generation is kept from the previous user, so ids
    // of removed sources never match it again
    Source &source = _sources[index];
    source.handler = handler;
    source.opaque = opaque;
    source.used = true;
    return SourceId{ index, source.generation };
}

/**
 * Frees the slot of a source if the id still names its current user.
 */
void Loop::removeSource(SourceId id) {
    // an index outside the table names nothing
    if (id.index < 0 || id.index >= _sourceCount) {
        return;
    }

    // free the slot and bump its generation: the messages of the old id
    // still queued no longer match, dispatch() drops them
    Source &source = _sources[id.index];
    if (source.used && source.generation == id.generation) {
        source.used = false;
        source.generation++;
    }
}

/**
 * Forwards to the mailbox. Any thread.
 */
void Loop::post(SourceId source, int kind, uint8_t *data, size_t size, int64_t value) {
    _mailbox->post(source, kind, data, size, value);
}

/**
 * Gives a message to the handler of its source, then frees the message and
 * its data. JS thread.
 */
void Loop::dispatch(Message *message) {
    // the source must still exist with the same generation: a message of a
    // removed source is dropped
    int index = message->source.index;
    bool valid = index >= 0 && index < _sourceCount;
    if (valid) {
        Source &source = _sources[index];
        valid = source.used && source.generation == message->source.generation;
        if (valid) {
            // call the handler inside a CallScope, which catches the errors
            // of js::fail and the JS exceptions: report them as uncaught
            js::detail::CallScope scope(_ctx);
            source.handler(source.opaque, message);
            if (scope.failed) {
                reportUncaught();
            }
        }
    }

    // free the message and its data in every case
    memory::release(message->data);
    memory::release(message);
}

// ### timers ##################################################################

/**
 * Inserts a timer in the list sorted by deadline, after the timers due at
 * the same time: they run in creation order.
 */
void Loop::insertTimer(Timer *timer) {
    // walk to the first timer due later than this one
    Timer **link = &_firstTimer;

    while (*link != nullptr && (*link)->deadline <= timer->deadline) {
        link = &(*link)->next;
    }

    // link the new one before it
    timer->next = *link;
    *link = timer;
}

/**
 * Frees a timer (already unlinked) and its JS values.
 */
void Loop::freeTimer(Timer *timer) {
    // release the callback and the arguments
    JS_FreeValue(_ctx, timer->callback);

    for (int i = 0; i < timer->argc; i++) {
        JS_FreeValue(_ctx, timer->argv[i]);
    }

    // then the memory
    memory::release(timer->argv);
    memory::release(timer);
}

/**
 * Creates a timer (setTimeout / setInterval) and inserts it in the sorted
 * list. Returns its id.
 */
int Loop::addTimer(int64_t delayMs, bool repeat, js::Function callback, js::Args arguments) {
    // a negative delay means now
    if (delayMs < 0) {
        delayMs = 0;
    }

    // build the timer: new id, deadline, interval (-1 if not repeating, at
    // least 1 ms otherwise so that an interval of 0 cannot starve the loop),
    // a reference to the callback, room for the arguments
    Timer *timer = (Timer *)memory::allocate(sizeof(Timer));
    timer->id = _nextTimerId++;
    timer->deadline = platform::monotonicMs() + delayMs;
    timer->interval = repeat ? (delayMs > 0 ? delayMs : 1) : -1;
    timer->callback = callback.dup();
    timer->argc = arguments.count;
    timer->argv = (JSValue *)memory::allocate(sizeof(JSValue) * (arguments.count > 0 ? arguments.count : 1));

    // keep a reference to each argument (freeTimer() releases them)
    for (int i = 0; i < arguments.count; i++) {
        timer->argv[i] = JS_DupValue(_ctx, arguments.values[i]);
    }

    // insert it in the sorted list
    insertTimer(timer);
    return timer->id;
}

/**
 * Cancels a timer (clearTimeout / clearInterval), also from its own
 * callback. Ignores an unknown id.
 */
void Loop::clearTimer(int id) {
    // the running timer is not in the list (runExpiredTimers() unlinked it):
    // only mark it cleared, runExpiredTimers() then frees it instead of
    // re-arming it
    if (id == runningTimerId) {
        runningTimerCleared = true;
        return;
    }

    // any other timer: unlink it and free it
    for (Timer **link = &_firstTimer; *link != nullptr; link = &(*link)->next) {
        Timer *timer = *link;
        if (timer->id == id) {
            *link = timer->next;
            freeTimer(timer);
            return;
        }
    }
}

/**
 * Runs the timers whose deadline has passed, in deadline order, and re-arms
 * the repeating ones.
 */
void Loop::runExpiredTimers() {
    // read 'now' once: a re-armed timer is due after it, so the loop below
    // ends (a new 0 ms timer may still run in this pass, until the clock
    // passes 'now')
    int64_t now = platform::monotonicMs();

    // for each expired timer, until exit() is called
    while (_firstTimer != nullptr && _firstTimer->deadline <= now && !_exitRequested) {
        // unlink it and mark it running (for clearTimer() from its callback)
        Timer *timer = _firstTimer;
        _firstTimer = timer->next;
        runningTimerId = timer->id;
        runningTimerCleared = false;

        // call it; an exception is reported as uncaught
        JSValue result = JS_Call(_ctx, timer->callback, JS_UNDEFINED, timer->argc, timer->argv);
        if (JS_IsException(result)) {
            reportUncaught();
        }

        JS_FreeValue(_ctx, result);
        runningTimerId = 0;

        // re-arm it from the current time if it repeats and was not cleared,
        // free it otherwise
        bool again = timer->interval >= 0 && !runningTimerCleared;
        if (again) {
            timer->deadline = platform::monotonicMs() + timer->interval;
            insertTimer(timer);
        } else {
            freeTimer(timer);
        }

        // run the promise jobs, as browsers do between two timers
        runJobs();
    }
}

/**
 * Milliseconds until the first timer is due (0 if expired), or -1 if there
 * is no timer.
 */
int64_t Loop::nextTimerDelay() {
    // no timer: wait for a message, without limit
    if (_firstTimer == nullptr) {
        return -1;
    }

    // the list is sorted: the first timer gives the delay
    int64_t delay = _firstTimer->deadline - platform::monotonicMs();
    return delay > 0 ? delay : 0;
}

// ### promise jobs and rejections #############################################

/**
 * Runs the pending promise jobs until there is none left or exit() is called;
 * a job that throws is reported as uncaught and the others still run.
 */
void Loop::runJobs() {
    JSRuntime *rt = JS_GetRuntime(_ctx);

    for (;;) {
        // no JS runs once the program has called exit()
        if (_exitRequested) {
            break;
        }

        // run the next job; 0: none left
        JSContext *jobContext = nullptr;
        int result = JS_ExecutePendingJob(rt, &jobContext);
        if (result == 0) {
            break;
        }

        // < 0: the job threw
        if (result < 0) {
            reportUncaught();
        }
    }
}

/**
 * Promise rejection tracker of the runtime: records a promise rejected
 * without handler, forgets it if it gets a handler later.
 */
void Loop::rejectionTracker(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                            JS_BOOL isHandled, void *opaque) {
    Loop *loop = (Loop *)opaque;

    // no handler: keep the promise and the reason in the list;
    // reportRejections() reports them once the jobs are drained
    if (!isHandled) {
        Rejection *rejection = (Rejection *)memory::allocate(sizeof(Rejection));
        rejection->promise = JS_DupValue(ctx, promise);
        rejection->reason = JS_DupValue(ctx, reason);
        rejection->next = loop->_firstRejection;
        loop->_firstRejection = rejection;
        return;
    }

    // a handler was added after all: find the promise (by pointer) and
    // forget its rejection
    for (Rejection **link = &loop->_firstRejection; *link != nullptr; link = &(*link)->next) {
        Rejection *rejection = *link;
        bool samePromise = JS_VALUE_GET_PTR(rejection->promise) == JS_VALUE_GET_PTR(promise);
        if (samePromise) {
            *link = rejection->next;
            JS_FreeValue(ctx, rejection->promise);
            JS_FreeValue(ctx, rejection->reason);
            memory::release(rejection);
            return;
        }
    }
}

/**
 * Reports the rejections still without handler ("Uncaught (in promise)").
 * Called once the jobs are drained: a job can no longer add a handler.
 * Nothing after exit(): shutdown() frees them unreported.
 */
void Loop::reportRejections() {
    // after exit(), a rejection is only the program being unwound (an async
    // function stopped by process.exit), and reporting could change the code
    if (_exitRequested) {
        return;
    }

    // each rejection still listed has no handler
    while (_firstRejection != nullptr) {
        // unlink it
        Rejection *rejection = _firstRejection;
        _firstRejection = rejection->next;

        // throw its reason again so that js::reportException() prints it,
        // then free it
        JS_Throw(_ctx, JS_DupValue(_ctx, rejection->reason));
        js::reportException(_ctx, "Uncaught (in promise)");
        JS_FreeValue(_ctx, rejection->promise);
        JS_FreeValue(_ctx, rejection->reason);
        memory::release(rejection);

        // with exitOnUncaught, ask the loop to stop with code 1 (Loop::exit,
        // not the C library's)
        if (_exitOnUncaught) {
            exit(1);
        }
    }
}

/**
 * Prints and clears the pending exception; with exitOnUncaught, asks the
 * loop to stop with code 1 (Loop::exit, not the C library's). After exit(),
 * only clears it.
 */
void Loop::reportUncaught() {
    // after exit(), the exception is the one process.exit throws to unwind
    // the running JS: drop it silently, the exit code stays
    if (_exitRequested) {
        JS_FreeValue(_ctx, JS_GetException(_ctx));
        return;
    }

    js::reportException(_ctx);
    if (_exitOnUncaught) {
        exit(1);
    }
}

// ### running #################################################################

/**
 * Records the exit request: run() stops at its next check.
 */
void Loop::exit(int code) {
    _exitRequested = true;
    _exitCode = code;
}

/**
 * True while something can still produce a callback: a queued message, a
 * promise job, a timer, a JS object kept alive (pending I/O) or a hold.
 */
bool Loop::hasWork() {
    bool jobs = JS_IsJobPending(JS_GetRuntime(_ctx)) != 0;
    return !_mailbox->empty() || jobs || _firstTimer != nullptr || js::keptAliveCount() > 0 || _holds > 0;
}

/**
 * Runs the loop until exit() is called or nothing can happen any more.
 * JS thread. Returns the exit code.
 */
int Loop::run() {
    // become the current loop of the thread
    currentLoop = this;

    while (!_exitRequested) {
        // run the promise jobs, free the objects released by keepAlive(false)
        // (outside of any of their methods), report the unhandled rejections
        // (only now: a job may still add a handler)
        runJobs();
        js::collectReleased();
        reportRejections();

        // stop if exit() was called
        if (_exitRequested) {
            break;
        }

        // run the expired timers (each one followed by its promise jobs)
        runExpiredTimers();

        // report the rejections the timers left before the check below: if
        // the loop stops there, shutdown() would free them unreported (not
        // after exit(): reporting could change its exit code)
        if (!_exitRequested) {
            reportRejections();
        }

        // stop if exit() was called or nothing can happen any more
        if (_exitRequested || !hasWork()) {
            break;
        }

        // wait for a message until the next timer is due, then dispatch it
        // (nullptr: a timer is due, or an early wake up)
        Message *message = _mailbox->take(nextTimerDelay());
        if (message != nullptr) {
            dispatch(message);
        }
    }

    return _exitCode;
}

} // namespace rt
