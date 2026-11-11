/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

#include "runner.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "debugger.h"
#include "loop.h"
#include "modules.h"
#include "platform.h"
#include "program.h"

namespace agent {

namespace {

/**
 * Runs the installed package on the JS thread, on request of the agent.
 *
 * Requests (RUN, STOP, RESET) come from the transport threads: they only
 * set flags under _signal and wake the JS thread. Stopping a running
 * program takes two ways at once:
 *   - the interrupt handler of the runtime sees _stopRequested and stops
 *     JS code that never returns to the loop;
 *   - a message to the loop (_controlSource) ends the event loop when the
 *     program is idle.
 * The debugger is told too (abortRun): a program paused in the debugger
 * leaves its pause loop.
 */
class Runner : public Controller {
public:
    /**
     * Gives the agent and the debugger (not owned); called once, before any
     * request can arrive.
     */
    void attach(Agent *agent, Debugger *debugger) {
        _agent = agent;
        _debugger = debugger;
    }

    /**
     * Asks for a run (transport thread, or the JS thread at boot): the
     * running program is stopped first, mainLoop() runs the package again.
     */
    void run() override {
        // under _signal: ask for a run, stop the running program, wake mainLoop()
        _signal.lock();
        _runRequested = true;
        stopLocked();
        _signal.notify();
        _signal.unlock();
    }

    /**
     * Transport thread: cancels a pending run request and stops the running
     * program, if any.
     */
    void stop() override {
        _signal.lock();
        _runRequested = false;
        stopLocked();
        _signal.unlock();
    }

    /**
     * A target restarts the chip; the desktop runs the program again.
     */
    void reset() override {
        if (!board::restart()) {
            run();
        }
    }

    /**
     * True while a run is in progress (from the run request taken to EXITED).
     */
    bool running() override {
        _signal.lock();
        bool isRunning = _running;
        _signal.unlock();
        return isRunning;
    }

    /**
     * JS thread: waits for run requests and runs the package, forever.
     */
    void mainLoop() {
        for (;;) {
            // wait for a run request (under _signal)
            _signal.lock();

            while (!_runRequested) {
                _signal.wait(-1);
            }

            // clear it before the run: a RUN arriving during the run runs the
            // package again afterwards
            _runRequested = false;

            // running from now on, in the same critical section: a STOP from
            // here on stops this run (stopLocked() sees _running), even
            // before its loop exists; no stop requested yet, the debugger
            // forgets the abort of the previous run
            __atomic_store_n(&_stopRequested, 0, __ATOMIC_RELEASE);
            _running = true;
            _debugger->prepareRun();
            _signal.unlock();

            // run, outside the lock
            int code = 0;
            bool started = runOnce(&code);

            // not running any more; a run that started tells the host (EXITED)
            _signal.lock();
            _running = false;
            _signal.unlock();

            if (started) {
                _agent->sendExited(code);
            }
        }
    }

private:
    /**
     * Asks the running program to stop; called with _signal held. No-op when
     * nothing runs.
     */
    void stopLocked() {
        // nothing runs: nothing to stop
        if (!_running) {
            return;
        }

        // stop JS code at the next interrupt, leave a debugger pause, and end
        // the event loop if it exists (idle program)
        __atomic_store_n(&_stopRequested, 1, __ATOMIC_RELEASE);
        _debugger->abortRun();
        if (_mailbox != nullptr) {
            _mailbox->post(_controlSource, 0, nullptr, 0, 0);
        }
    }

    /**
     * One run of the installed package (marked running by mainLoop()), from
     * STARTED to its end; *exitCode receives the exit code. False if no
     * package is installed (nothing started).
     */
    bool runOnce(int *exitCode) {
        // open the package (read in place)
        size_t size = 0;
        const uint8_t *package = board::openPackage(&size);
        if (package == nullptr) {
            fprintf(stderr, "qjsx agent: no package installed\n");
            return false;
        }

        // its build id, for STARTED
        char buildId[64] = "";
        size_t installedSize = 0;
        board::packageStorage()->installed(buildId, sizeof(buildId), &installedSize);

        // tell the host (STARTED)
        _agent->sendStarted(buildId);

        // the hooks of the run (below)
        rt::RunHooks hooks = {};
        hooks.opaque = this;
        hooks.started = &started;
        hooks.finished = &finished;
        hooks.interrupt = &interrupt;
        hooks.exitOnUncaught = false;   // a device prints the exception and goes on
        hooks.moduleLoaded = &moduleLoaded;
        hooks.device = true;            // argv and env come from the package manifest

        // run it until it ends or is stopped, then close the package
        *exitCode = rt::runPackage(package, size, &hooks, 0, nullptr);
        board::closePackage(package);
        return true;
    }

    // ### hooks of the run (JS thread) ########################################

    /**
     * Loop created, nothing evaluated yet: lets stopLocked() end the loop and
     * starts the debugger on it.
     */
    static void started(void *opaque, rt::Loop *loop) {
        // keep the loop for onControl() (JS thread only)
        Runner *runner = (Runner *)opaque;
        runner->_loop = loop;

        // under _signal: retain the mailbox and add the control source, so
        // that stopLocked() can now end the loop
        runner->_signal.lock();
        runner->_mailbox = loop->mailbox();
        runner->_mailbox->retain();
        runner->_controlSource = loop->addSource(&onControl, runner);

        // a stop requested while the run was starting (no loop yet, nothing
        // posted then): end the loop now
        bool stopPending = __atomic_load_n(&runner->_stopRequested, __ATOMIC_ACQUIRE) != 0;
        if (stopPending) {
            runner->_mailbox->post(runner->_controlSource, 0, nullptr, 0, 0);
        }

        runner->_signal.unlock();

        // start the debugger on this loop
        runner->_debugger->start(loop->context(), loop);
    }

    /**
     * Run ended (context still alive): finishes the debugger, releases the
     * mailbox.
     */
    static void finished(void *opaque) {
        // finish the debugger first: it frees its JS values while the context
        // is alive
        Runner *runner = (Runner *)opaque;
        runner->_debugger->finish();

        // under _signal: release the mailbox; stopLocked() no longer posts
        runner->_signal.lock();
        runner->_mailbox->release();
        runner->_mailbox = nullptr;
        runner->_signal.unlock();

        runner->_loop = nullptr;
    }

    /**
     * Interrupt handler of the runtime: runs the light debugger requests,
     * lets the other tasks of the core run from time to time, then returns
     * non-zero to stop the JS code if a stop was requested.
     */
    static int interrupt(JSRuntime *, void *opaque) {
        Runner *runner = (Runner *)opaque;
        runner->_debugger->poll();   // debugger requests that run no JS code

        // JS code that computes for a long time must not starve the idle task
        // of its core (task watchdog of the RTOS); no-op on the desktop
        platform::yieldIfDue();

        // a stop was requested: end the loop first, so that the exception
        // QuickJS throws to stop the JS is dropped silently (as after
        // process.exit) instead of being reported as uncaught
        bool stop = __atomic_load_n(&runner->_stopRequested, __ATOMIC_ACQUIRE) != 0;
        if (stop && runner->_loop != nullptr) {
            runner->_loop->exit(0);
        }

        return stop ? 1 : 0;
    }

    /**
     * A module was loaded, not evaluated yet: tells the debugger (which may
     * wait for the host there).
     */
    static void moduleLoaded(void *opaque, const char *name) {
        ((Runner *)opaque)->_debugger->moduleLoaded(name);
    }

    /**
     * Loop message posted by stopLocked(): ends the event loop.
     */
    static void onControl(void *opaque, const rt::Message *) {
        ((Runner *)opaque)->_loop->exit(0);
    }

    Agent           *_agent = nullptr;            // borrowed, set by attach()
    Debugger        *_debugger = nullptr;         // borrowed, set by attach()
    platform::Signal _signal;                     // protects the fields below
    bool             _runRequested = false;       // mainLoop() must run the package
    bool             _running = false;            // from the run request taken to EXITED
    rt::Mailbox     *_mailbox = nullptr;          // of the running loop, retained; nullptr: none
    rt::SourceId     _controlSource = { -1, 0 };  // onControl() in that loop; id -1: none
    int              _stopRequested = 0;          // atomic
    rt::Loop        *_loop = nullptr;             // JS thread only
};

/**
 * Console output of the program: to the host during a session, else to
 * the console of the board.
 */
void outputToHost(void *opaque, int stream, const char *text, size_t length) {
    Agent *agent = (Agent *)opaque;
    if (!agent->sendLog(stream, text, length)) {
        board::writeConsole(stream, text, length);
    }
}

} // namespace

/**
 * Runs the device: wires the agent, the runner and the debugger, starts the
 * links, then the calling thread becomes the JS thread (see runner.h).
 */
void runDevice(Identity identity) {
    // build the runner, the agent and the debugger (on this stack: they live
    // as long as the device) and connect them: the agent answers the host
    // with the storage of the board and the runner, the debugger requests go
    // to the stub, the console output goes to the host
    Runner runner;
    Agent device(board::packageStorage(), &runner, identity);
    Debugger debugger(&device);
    device.setDebugger(&debugger);
    runner.attach(&device, &debugger);
    rt::setOutputSink(&outputToHost, &device);

    // the programs keep their files in the board's data directory (their
    // process.cwd())
    rt::setDataDirectory(board::dataDirectory());

    // start the links; the device cannot work without them. On failure the
    // output goes back to the console first: the sink points to the agent,
    // destroyed on return
    if (!board::startLinks(&device)) {
        rt::setOutputSink(nullptr, nullptr);
        return;
    }

    // like a device: the installed package starts at boot
    char buildId[64];
    size_t size = 0;
    if (board::packageStorage()->installed(buildId, sizeof(buildId), &size)) {
        runner.run();
    }

    // run the packages the host asks for, forever
    runner.mainLoop();
}

} // namespace agent
