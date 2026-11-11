/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Event loop of the runtime (same code on the host and on the ESP).
//
// One JS thread runs the loop. Event producers (I/O threads, drivers,
// interrupts on the ESP) never touch JS: they post messages in the loop's
// mailbox. The loop:
//   1. runs the pending promise jobs,
//   2. runs the expired timers,
//   3. waits for the next message (or the next timer),
//   4. gives the message to the handler of its source, on the JS thread,
// and stops when nothing can happen any more: no timer, no message, no object
// kept alive (js::keepAlive), no hold (Loop::hold).
//
// A message names its source by index + generation, never by pointer: once a
// source is removed, its messages still in the queue are dropped.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"
#include "memory.h"
#include "jsbind.h"
#include "platform.h"

namespace rt {

struct SourceId {
    int      index;       // slot in the loop's source table
    uint32_t generation;  // generation of the slot when the id was made
};

struct Message {
    SourceId source;  // source that posted it (its handler receives it)
    int      kind;    // meaning chosen by the source
    uint8_t *data;    // allocated with malloc, freed by the loop after dispatch
    size_t   size;    // bytes in data
    int64_t  value;   // meaning chosen by the source
    Message *next;    // next in the mailbox queue; nullptr: last
};

// Called on the JS thread.
typedef void MessageHandler(void *opaque, const Message *message);

// Message queue shared by the loop and the producer threads. Reference
// counted: a producer may keep it after the loop is destroyed; its messages
// are then dropped instead of reaching freed memory.
class Mailbox {
public:
    /**
     * Empty and open, with one reference (the loop's).
     */
    Mailbox();

    /**
     * Any thread: adds a reference (a producer thread keeping the mailbox).
     */
    void             retain();

    /**
     * Any thread: drops a reference; the last one frees the mailbox.
     */
    void             release();

    /**
     * Queues a message for the JS thread. Any thread; 'data' must come from
     * memory::allocate() and the mailbox owns it (freed at once if the mailbox is closed).
     */
    void             post(SourceId source, int kind, uint8_t *data, size_t size, int64_t value);

    /**
     * JS thread: next message, waiting at most timeoutMs (< 0: forever). The
     * caller owns the message and its data, and loops: nullptr may come early.
     */
    Message         *take(int64_t timeoutMs);

    /**
     * Any thread: true when no message is queued.
     */
    bool             empty();

    /**
     * The loop is going away: drop the queued and the future messages.
     */
    void             close();

private:
    /**
     * Private: only release() deletes a mailbox. Frees the queued messages.
     */
    ~Mailbox();

    /**
     * Frees the queued messages and their data. Lock held (or last reference).
     */
    void             dropAll();

    platform::Signal _signal;      // lock + condition guarding the fields below
    Message         *_first;       // oldest queued message; nullptr: empty (lock)
    Message         *_last;        // newest queued message (lock)
    bool             _closed;      // true: post() drops the messages (lock)
    int              _references;  // the loop + producer threads; atomic
};

class Loop {
public:
    /**
     * Loop of 'ctx' on the calling (JS) thread: becomes Loop::current() and
     * installs the promise rejection tracker of the runtime.
     */
    explicit Loop(JSContext *ctx);

    /**
     * Closes the mailbox and frees the sources. No JS: call shutdown()
     * before JS_FreeContext, delete the loop after JS_FreeRuntime.
     */
    ~Loop();
    Loop(const Loop &) = delete;
    Loop &operator=(const Loop &) = delete;

    /**
     * The loop of the calling JS thread.
     */
    static Loop *current();

    /**
     * The context the loop runs JS in.
     */
    JSContext   *context() const {
        return _ctx;
    }

    /**
     * For producer threads: retain() it before giving it to a thread.
     */
    Mailbox     *mailbox() const {
        return _mailbox;
    }

    // ### sources (JS thread) #################################################

    /**
     * Registers a message handler (called on the JS thread with 'opaque').
     * The id goes with the messages the producers post for it.
     */
    SourceId     addSource(MessageHandler *handler, void *opaque);

    /**
     * Unregisters a source; its messages still queued are dropped. Ignores
     * an unknown or already removed id.
     */
    void         removeSource(SourceId source);

    /**
     * Any thread (same as mailbox()->post()).
     */
    void         post(SourceId source, int kind, uint8_t *data, size_t size, int64_t value);

    // ### timers (JS thread) ##################################################

    /**
     * setTimeout / setInterval: calls 'callback' with 'arguments' after
     * delayMs (negative = 0), then every delayMs if 'repeat' (at least 1 ms).
     * Returns the timer id (> 0).
     */
    int          addTimer(int64_t delayMs, bool repeat, js::Function callback, js::Args arguments);

    /**
     * clearTimeout / clearInterval, also from the timer's own callback.
     * Ignores an unknown id.
     */
    void         clearTimer(int id);

    /**
     * Keeps the loop running without any JS object (C++ services like the
     * device agent). JS thread.
     */
    void         hold() {
        _holds++;
    }

    /**
     * Ends one hold().
     */
    void         unhold() {
        _holds--;
    }

    /**
     * Runs until there is nothing left to do or exit() is called.
     * Returns the exit code.
     */
    int          run();

    /**
     * Stops the loop (process.exit).
     */
    void         exit(int code);

    /**
     * True once exit() was called.
     */
    bool         exitRequested() const {
        return _exitRequested;
    }

    /**
     * Uncaught exceptions: print and stop with code 1 (host), or print and
     * continue (ESP).
     */
    void         setExitOnUncaught(bool exitOnUncaught) {
        _exitOnUncaught = exitOnUncaught;
    }

    /**
     * Reports the exception pending in the context.
     */
    void         reportUncaught();

    /**
     * Frees the JS values held by the loop (timers, rejections). Call it before
     * JS_FreeContext; delete the loop after JS_FreeRuntime (finalizers may
     * still remove their sources).
     */
    void         shutdown();

private:
    struct Timer;
    struct Source {
        MessageHandler *handler;     // called on the JS thread
        void           *opaque;      // passed to handler; borrowed
        uint32_t        generation;  // bumped when the slot is freed: stale ids are rejected
        bool            used;        // false: free slot
    };
    struct Rejection;

    /**
     * Runs the pending promise jobs until there is none left.
     */
    void         runJobs();

    /**
     * Runs the timers whose deadline has passed, re-arms the repeating ones.
     */
    void         runExpiredTimers();

    /**
     * Milliseconds until the first timer (0 if expired), or -1 if none.
     */
    int64_t      nextTimerDelay();

    /**
     * Gives a message to the handler of its source (if that source still
     * exists), then frees the message and its data.
     */
    void         dispatch(Message *message);

    /**
     * Prints the rejections left without a handler.
     */
    void         reportRejections();

    /**
     * Inserts a timer in the list, sorted by deadline.
     */
    void         insertTimer(Timer *timer);

    /**
     * Frees a timer (unlinked) and its JS values.
     */
    void         freeTimer(Timer *timer);

    /**
     * True while something can still happen (message, job, timer, object
     * kept alive, hold).
     */
    bool         hasWork();

    /**
     * Promise rejection tracker of the runtime: records the unhandled
     * rejections, forgets those that get a handler later.
     */
    static void rejectionTracker(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                                 JS_BOOL isHandled, void *opaque);

    JSContext   *_ctx;             // borrowed
    Mailbox     *_mailbox;         // one reference held, released by ~Loop
    Source      *_sources;         // malloc'd, indexed by SourceId.index
    int          _sourceCount;     // slots in _sources (used or free)
    Timer       *_firstTimer;      // sorted by deadline; owned; nullptr: none
    int          _nextTimerId;     // id of the next timer (starts at 1)
    Rejection   *_firstRejection;  // rejections still without handler; owned
    int          _holds;           // hold() calls not ended by unhold()
    bool         _shutDown;        // true once shutdown() ran
    bool         _exitRequested;   // true once exit() was called
    int          _exitCode;        // given to exit(); 0 by default
    bool         _exitOnUncaught;  // true: an uncaught exception stops the loop (code 1)
};

} // namespace rt
