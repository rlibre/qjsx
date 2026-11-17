/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "stdlib/tasks" of the standard library (lib/stdlib): small tasks
// written as plain sequential code that waits, instead of state machines
// called by timers. Everything runs on the one JS thread: a task is an async
// function, the other tasks and the callbacks run while it waits.
//
//   import { task, Mutex } from "stdlib/tasks";
//
//   const blink = task("blink", async self => {
//       const tick = self.every(500);
//       while (!self.stopped) {
//           led.toggle();
//           await tick.next();
//       }
//       led.write(0);                 // after the loop: the task is ending
//   });
//
//   blink.stop();                     // asks: self.stopped, the waits end at once
//   await blink.join();               // the end of the task
//
// Stopping is cooperative: stop() sets self.stopped and ends at once the
// waits of the task made by this module (self.sleep, tick.next, the
// mailbox); the task leaves its loop and finishes its own way.

// Time after stop() before a task that has not finished is reported.
const STOP_WARNING_DELAY = 5000;

// Methods of a task kept for Ticker and Mailbox: symbols of this module, so
// that a program does not see them on its tasks.
const WAIT = Symbol("wait");
const ON_STOP = Symbol("onStop");

/**
 * Throws a TypeError unless 'value' is a number > 0 ('what' names it).
 */
function checkDuration(value, what) {
    // !(value > 0) also refuses NaN
    const isBadDuration = typeof value !== "number" || !(value > 0);
    if (isBadDuration) {
        throw new TypeError(`tasks: ${what} must be a number of milliseconds > 0`);
    }
}

// ### Task ####################################################################

class Task {
    #name;                          // given to task(), printed with its errors
    #stopped = false;               // stop() was called
    #finished = false;              // the body has returned or thrown
    #waiters = new Set();           // waits of [WAIT]() in progress: { resolve, timer }
    #wakers = [];                   // functions of [ON_STOP]() (mailboxes, tickers), called by stop()
    #warningTimer = null;           // the report of a late end, set by stop()
    #outcome;                       // promise of { value } or { error }, never rejected

    /**
     * Runs 'body(this)' at once (until its first wait, as any async
     * function called).
     */
    constructor(name, body) {
        this.#name = name;

        // the body, its error printed; a body that is not async is run the
        // same way (its exception becomes the error)
        const run = (async () => body(this))();
        this.#outcome = run.then(value => this.#end({ value }), error => this.#end({ error, failed: true }));
    }

    /**
     * The name given to task().
     */
    get name() {
        return this.#name;
    }

    /**
     * True once stop() was called (the body then leaves its loop).
     */
    get stopped() {
        return this.#stopped;
    }

    /**
     * Asks the task to stop: 'stopped' becomes true, every wait of the task
     * made by this module ends at once. Reports the task if it has not
     * finished after STOP_WARNING_DELAY. Does nothing the second time.
     */
    stop() {
        // asked already: nothing more to do
        if (this.#stopped) {
            return;
        }

        this.#stopped = true;

        // the waits in progress end now (a copy: #endWait() changes the set)
        for (const waiter of [...this.#waiters]) {
            this.#endWait(waiter);
        }

        // then the receiving of the mailboxes and the steps of the tickers
        const wakers = this.#wakers.splice(0);
        for (const wake of wakers) {
            wake();
        }

        // a task that does not look at 'stopped' is reported
        if (!this.#finished) {
            this.#warningTimer = setTimeout(() => {
                console.warn(`${this.#name}: not finished ${STOP_WARNING_DELAY / 1000} s after stop()`);
            }, STOP_WARNING_DELAY);
        }
    }

    /**
     * The end of the task: resolved with the value of the body, or rejected
     * with its error (which the task has printed already).
     */
    join() {
        return this.#outcome.then(outcome => {
            if (outcome.failed) {
                throw outcome.error;
            }

            return outcome.value;
        });
    }

    /**
     * A promise resolved after 'ms' milliseconds, or at once once stopped.
     */
    sleep(ms) {
        checkDuration(ms, "the time of sleep()");
        return this[WAIT](ms);
    }

    /**
     * A ticker of period 'period' milliseconds, counted from now.
     */
    every(period) {
        checkDuration(period, "the period of every()");
        return new Ticker(this, period);
    }

    /**
     * A mailbox of 'size' values at most, read by this task.
     */
    mailbox(size) {
        const isBadSize = !Number.isInteger(size) || size < 1;
        if (isBadSize) {
            throw new TypeError("tasks: the size of a mailbox must be an integer >= 1");
        }

        return new Mailbox(this, size);
    }

    /**
     * For sleep(): a promise resolved after 'ms' milliseconds, or at once
     * when the task is stopped.
     */
    [WAIT](ms) {
        return new Promise(resolve => {
            // stopped already: no wait at all
            if (this.#stopped) {
                resolve();
                return;
            }

            // the timer and the stop end the wait the same way. No function
            // here refers to itself: such a cycle is freed only by the
            // cycle collector, and a ticker of 10 ms would fill a small heap
            // with them before it runs
            const waiter = { resolve, timer: null };
            waiter.timer = setTimeout(() => this.#endWait(waiter), ms);
            this.#waiters.add(waiter);
        });
    }

    /**
     * Ends a wait of [WAIT](): its timer cleared, forgotten, resolved.
     */
    #endWait(waiter) {
        clearTimeout(waiter.timer);
        this.#waiters.delete(waiter);
        waiter.resolve();
    }

    /**
     * For Mailbox and Ticker: calls 'wake' when the task is stopped (at once
     * if it is already).
     */
    [ON_STOP](wake) {
        if (this.#stopped) {
            wake();
            return;
        }

        this.#wakers.push(wake);
    }

    /**
     * The body has ended: its error printed, the report of a late end
     * dropped. Returns 'outcome' for join().
     */
    #end(outcome) {
        this.#finished = true;
        clearTimeout(this.#warningTimer);

        // said plainly: the task is over, unlike an error a loop prints and
        // goes on after
        if (outcome.failed) {
            console.error(`${this.#name}: ended by an error:`, outcome.error);
        }

        return outcome;
    }
}

// ### Ticker ##################################################################

// The steps of a ticker are at fixed times: its start + period, + 2 periods...
// A step already passed when next() is called ends the wait at the next turn
// of the loop, and the steps passed before it are skipped: a late loop does
// not run several times in a row to catch up. Times are those of Date.now().
//
// A step costs one promise and one timer, nothing else (measured on a board:
// the wait was a third of a program's time): the ticker keeps its resolve
// function and its timer, its timer callback is made once, and it registers
// once with its task, which ends its wait when stopped. So a ticker lives as
// long as its task: make it once, before the loop.
class Ticker {
    #task;                          // the task it belongs to
    #period;                        // ms between two steps
    #nextStep;                      // time of the next step (Date.now())
    #promise = null;                // of the step waited for; null: none
    #resolve = null;                // its resolve function
    #timer = null;                  // its timer
    #fire;                          // the timer callback, made once

    /**
     * A ticker of 'owner' whose first step is 'period' ms from now.
     */
    constructor(owner, period) {
        this.#task = owner;
        this.#period = period;
        this.#nextStep = Date.now() + period;
        this.#fire = () => this.#settle();

        // registered once: the stop of the task ends the step waited for
        owner[ON_STOP](() => this.#interrupt());
    }

    /**
     * Waits for the next step; at once if the task is stopped. A second
     * call while a step is waited for gives the same step.
     */
    next() {
        // stopped: no wait; a step waited for already: the same one
        if (this.#task.stopped) {
            return Promise.resolve();
        }

        if (this.#promise !== null) {
            return this.#promise;
        }

        // the step this call waits for, then the one after it; late: the
        // steps passed are skipped, the next one is the first after now
        const now = Date.now();
        const delay = this.#nextStep - now;
        if (delay > 0) {
            this.#nextStep += this.#period;
        } else {
            const passed = Math.floor((now - this.#nextStep) / this.#period) + 1;
            this.#nextStep += passed * this.#period;
        }

        // the wait: a promise and its timer
        const { promise, resolve } = Promise.withResolvers();
        this.#promise = promise;
        this.#resolve = resolve;
        this.#timer = setTimeout(this.#fire, delay > 0 ? delay : 0);
        return promise;
    }

    /**
     * The task stops: the step waited for, if any, ends now.
     */
    #interrupt() {
        if (this.#promise !== null) {
            clearTimeout(this.#timer);
            this.#settle();
        }
    }

    /**
     * Ends the wait of the step (from its timer, or the stop).
     */
    #settle() {
        // the step is forgotten (the next next() makes a new one), then resolved
        const resolve = this.#resolve;
        this.#promise = null;
        this.#resolve = null;
        this.#timer = null;
        resolve();
    }
}

// ### Mailbox #################################################################

// Values given by send() (often from a callback), taken in order by the task.
// send() never waits: a full mailbox refuses the value. Once the task is
// stopped, the values waiting are dropped and the receiving ends.
class Mailbox {
    #task;                          // the task that reads it
    #size;                          // values it holds at most
    #values = [];                   // the values waiting, oldest first
    #receivers = [];                // resolve functions of receive() calls waiting, in order

    /**
     * A mailbox of 'owner' holding 'size' values at most.
     */
    constructor(owner, size) {
        this.#task = owner;
        this.#size = size;

        // the stop drops the values waiting and ends the receiving
        owner[ON_STOP](() => {
            this.#values.length = 0;

            const receivers = this.#receivers.splice(0);
            for (const resolve of receivers) {
                resolve(null);
            }
        });
    }

    /**
     * Gives 'value' to the task. False when the mailbox is full or the task
     * stopped: the value is dropped.
     */
    send(value) {
        // stopped: refused
        if (this.#task.stopped) {
            return false;
        }

        // a receive() waiting takes it at once
        const receiver = this.#receivers.shift();
        if (receiver !== undefined) {
            receiver({ value });
            return true;
        }

        // else it waits its turn, if there is room
        if (this.#values.length >= this.#size) {
            return false;
        }

        this.#values.push(value);
        return true;
    }

    /**
     * The oldest value, waiting for one if there is none; undefined once
     * the task is stopped.
     */
    async receive() {
        const received = await this.#take();
        return received === null ? undefined : received.value;
    }

    /**
     * for await (const value of mailbox): the values in order, until the
     * task is stopped.
     */
    [Symbol.asyncIterator]() {
        return {
            next: async () => {
                const received = await this.#take();
                const isStopped = received === null;
                return isStopped ? { value: undefined, done: true } : { value: received.value, done: false };
            },
        };
    }

    /**
     * The oldest value as { value }, or null once the task is stopped (a
     * value sent may be undefined: the wrapper tells them apart).
     */
    #take() {
        // stopped: nothing more
        if (this.#task.stopped) {
            return Promise.resolve(null);
        }

        // a value waiting
        if (this.#values.length > 0) {
            return Promise.resolve({ value: this.#values.shift() });
        }

        // else the next send(), or the stop
        return new Promise(resolve => {
            this.#receivers.push(resolve);
        });
    }
}

// ### Exports #################################################################

/**
 * Starts a task: runs 'body(task)' at once, an async function given the
 * task as 'self'.
 */
export function task(name, body) {
    if (typeof name !== "string") {
        throw new TypeError("tasks: the name of a task must be a string");
    }

    if (typeof body !== "function") {
        throw new TypeError("tasks: the body of a task must be a function");
    }

    return new Task(name, body);
}

// Jobs run one at a time, in the order withLock() was called: what several
// tasks share (a bus, a port) is used by one of them at a time.
export class Mutex {
    #tail = Promise.resolve();      // settles when the last job given so far has ended

    /**
     * Runs 'job' once the jobs given before have ended; the promise settles
     * as the job's. An error of a job does not stop the next ones.
     */
    withLock(job) {
        if (typeof job !== "function") {
            throw new TypeError("tasks: withLock() needs a function");
        }

        // this job after the previous one, whatever its end
        const run = this.#tail.then(() => job());
        this.#tail = run.then(() => {}, () => {});
        return run;
    }
}
