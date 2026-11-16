/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The tasks of the standard library (stdlib/tasks): a task runs at once,
// join() gives its value or its error (printed with its name), stop() ends
// its waits at once (sleep, ticker, mailbox), a ticker keeps fixed times and
// skips the steps passed, a mailbox keeps the order and refuses when full,
// a mutex runs its jobs one at a time in order, and a task not finished
// 5 s after stop() is reported.
// Run with: bin/Release/qjsx run tests/tasks.test.js
import { check, equal, throws, rejects, report } from "./assert.js";
import { task, Mutex } from "stdlib/tasks";

// Margin of the timing checks, in milliseconds (a Debug build, a busy PC).
const SLACK = 40;

// What the tasks print, kept instead of written.
const printed = { errors: [], warnings: [] };
console.error = (...values) => {
    printed.errors.push(values.map(String).join(" "));
};
console.warn = (...values) => {
    printed.warnings.push(values.map(String).join(" "));
};

/**
 * A promise resolved after 'ms' milliseconds.
 */
function delay(ms) {
    return new Promise(resolve => {
        setTimeout(resolve, ms);
    });
}

/**
 * Computes until 'ms' milliseconds have passed (a loop that is late).
 */
function busy(ms) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
        // nothing: only the time passes
    }
}

// ### The task itself #########################################################

{
    // the body runs at once, until its first wait
    const steps = [];
    const first = task("first", async self => {
        steps.push("started");
        await self.sleep(10);
        steps.push("woken");
        return 42;
    });
    equal(steps.join(","), "started", "the body runs at once, until its first wait");
    equal(first.name, "first", "the name of the task");
    equal(first.stopped, false, "not stopped at first");
    equal(await first.join(), 42, "join() gives the value the body returned");
    equal(steps.join(","), "started,woken", "the body went on after its wait");
    equal(await first.join(), 42, "join() a second time gives the same value");
}

{
    // an error: printed with the name, join() rejects with it
    const failing = task("failing", async () => {
        await delay(1);
        throw new RangeError("out of range");
    });
    await rejects(failing.join(), /^RangeError: out of range$/, "join() rejects with the error of the body");
    check(printed.errors.some(text => text.startsWith("failing: ended by an error:") && text.includes("out of range")),
        "the error is printed with the name of the task, saying it ended");

    // a body that is not async: its exception is the error
    const synchronous = task("synchronous", () => {
        throw new TypeError("at once");
    });
    await rejects(synchronous.join(), /^TypeError: at once$/, "the exception of a body that is not async");
    printed.errors.length = 0;
}

throws(() => task(1, async () => {}), /^TypeError: tasks: the name/, "a name that is not a string");
throws(() => task("x"), /^TypeError: tasks: the body/, "a body that is not a function");

// ### stop() ##################################################################

{
    // stop() ends a long sleep at once, the task leaves its loop
    let turns = 0;
    const sleeper = task("sleeper", async self => {
        while (!self.stopped) {
            turns++;
            await self.sleep(10000);
        }
        return "done";
    });
    const start = Date.now();
    sleeper.stop();
    equal(sleeper.stopped, true, "stopped once stop() is called");
    equal(await sleeper.join(), "done", "the task ends its own way after stop()");
    check(Date.now() - start < SLACK, "stop() ends the sleep at once");
    equal(turns, 1, "the loop ran once");

    // after the stop, a sleep does not wait; a second stop does nothing
    const before = Date.now();
    await sleeper.sleep(10000);
    check(Date.now() - before < SLACK, "sleep() after the stop does not wait");
    sleeper.stop();
}

{
    // the arguments of the waits
    const owner = task("arguments", async () => {});
    throws(() => owner.sleep(0), /^TypeError: tasks: the time of sleep/, "sleep(0)");
    throws(() => owner.every(-1), /^TypeError: tasks: the period of every/, "every(-1)");
    throws(() => owner.every("10"), /^TypeError: tasks: the period of every/, "every('10')");
    throws(() => owner.mailbox(0), /^TypeError: tasks: the size of a mailbox/, "mailbox(0)");
    throws(() => owner.mailbox(1.5), /^TypeError: tasks: the size of a mailbox/, "mailbox(1.5)");
    await owner.join();
}

// ### Ticker ##################################################################

{
    // fixed times: the 20 ms of each turn do not shift the steps
    const times = [];
    const rhythm = task("rhythm", async self => {
        const start = Date.now();
        const tick = self.every(50);
        for (let step = 0; step < 4; step++) {
            busy(20);
            await tick.next();
            times.push(Date.now() - start);
        }
    });
    await rhythm.join();
    const expected = [50, 100, 150, 200];
    check(times.every((time, index) => Math.abs(time - expected[index]) < SLACK),
        `the steps at fixed times 50, 100, 150, 200 ms (got ${times.join(", ")})`);
}

{
    // late: the step passed ends the wait at once, the ones passed before it
    // are skipped, the next is back on the grid
    const times = [];
    const late = task("late", async self => {
        const start = Date.now();
        const tick = self.every(50);
        busy(130);
        await tick.next();
        times.push(Date.now() - start);
        await tick.next();
        times.push(Date.now() - start);
    });
    await late.join();
    check(times[0] < 130 + SLACK, `a late next() does not wait (got ${times[0]} ms)`);
    check(Math.abs(times[1] - 150) < SLACK, `the step after it is the next on the grid, 150 ms (got ${times[1]} ms)`);
}

{
    // a second next() while a step is waited for gives the same step
    const same = task("same step", async self => {
        const tick = self.every(30);
        const first = tick.next();
        const second = tick.next();
        await first;
        return first === second;
    });
    equal(await same.join(), true, "two next() in a row wait for the same step");
}

{
    // stop() ends a long wait of a ticker at once
    const slow = task("slow ticker", async self => {
        const tick = self.every(10000);
        while (!self.stopped) {
            await tick.next();
        }
    });
    const start = Date.now();
    slow.stop();
    await slow.join();
    check(Date.now() - start < SLACK, "stop() ends the wait of a ticker at once");
}

// ### Mailbox #################################################################

{
    // values sent before the task reads them: kept in order; full: refused
    let inbox = null;
    const received = [];
    const reader = task("reader", async self => {
        inbox = self.mailbox(3);
        await self.sleep(20);
        for await (const value of inbox) {
            received.push(value);
        }
        return "end";
    });
    equal(inbox.send("a"), true, "send() into a mailbox with room");
    equal(inbox.send(undefined), true, "undefined is a value like another");
    equal(inbox.send("c"), true, "the third value");
    equal(inbox.send("d"), false, "a full mailbox refuses the value");

    // the task reads them, then waits for the next one
    await delay(40);
    equal(received.map(String).join(","), "a,undefined,c", "the values in the order they were sent");
    equal(inbox.send("e"), true, "send() to a task waiting");
    await delay(1);
    equal(received[received.length - 1], "e", "the task waiting gets the value at once");

    // the stop ends the iteration; then nothing is taken
    reader.stop();
    equal(await reader.join(), "end", "the stop ends 'for await' over the mailbox");
    equal(inbox.send("f"), false, "send() after the stop refuses the value");
    equal(await inbox.receive(), undefined, "receive() after the stop gives undefined");
}

{
    // receive(); the values waiting at the stop are dropped
    let inbox = null;
    const values = [];
    const receiver = task("receiver", async self => {
        inbox = self.mailbox(5);
        values.push(await inbox.receive());
        await self.sleep(10000);
        values.push(await inbox.receive());
    });
    inbox.send(1);
    await delay(1);
    inbox.send(2);
    receiver.stop();
    await receiver.join();
    equal(values.map(String).join(","), "1,undefined", "receive() gives a value, then undefined: the value waiting at the stop is dropped");
}

// ### Mutex ###################################################################

{
    // jobs one at a time, in order; an error does not stop the next ones
    const bus = new Mutex();
    const order = [];
    let active = 0;
    let overlapped = false;

    /**
     * A job of 'ms' milliseconds that records its start and its end.
     */
    async function job(name, ms) {
        active++;
        overlapped = overlapped || active > 1;
        order.push(`${name}+`);
        await delay(ms);
        order.push(`${name}-`);
        active--;
        return name;
    }

    const first = bus.withLock(() => job("a", 20));
    const failing = bus.withLock(async () => {
        await delay(5);
        throw new Error("job failed");
    });
    const third = bus.withLock(() => job("c", 5));
    const plain = bus.withLock(() => "plain");

    equal(await first, "a", "withLock() gives the value of the job");
    await rejects(failing, /^Error: job failed$/, "withLock() rejects with the error of the job");
    equal(await third, "c", "the job after a failed one runs");
    equal(await plain, "plain", "a job that is not async");
    equal(order.join(","), "a+,a-,c+,c-", "the jobs in the order of the calls, one after the other");
    check(!overlapped, "two jobs never run at the same time");
    throws(() => bus.withLock(1), /^TypeError: tasks: withLock/, "withLock() of something that is not a function");
}

// ### The late end, reported ##################################################

{
    // a task that does not look at 'stopped', and one that ends at once
    printed.warnings.length = 0;
    const stubborn = task("stubborn", async () => {
        await delay(5600);
        return "late";
    });
    const prompt = task("prompt", async self => {
        await self.sleep(10000);
    });
    stubborn.stop();
    prompt.stop();
    await prompt.join();

    // 5 s later: only the stubborn one is reported, and join() still waits
    await delay(5300);
    equal(printed.warnings.length, 1, "one warning");
    check(printed.warnings[0]?.startsWith("stubborn: not finished 5 s after stop()"), "the warning names the late task");
    equal(await stubborn.join(), "late", "join() waits for the late task");
}

report("tasks");
