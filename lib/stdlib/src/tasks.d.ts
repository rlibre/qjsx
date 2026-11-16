/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * Small tasks written as plain sequential code that waits, from the standard
 * library (written in JS).
 *
 * A task is an async function given its {@link Task} as `self`: it loops,
 * waits for the next step ({@link Task.every}), for a time
 * ({@link Task.sleep}) or for values sent by callbacks
 * ({@link Task.mailbox}), and stops when asked ({@link Task.stopped}).
 * Everything runs on the one JS thread: the other tasks and the callbacks
 * run while a task waits, never in the middle of its code between two
 * waits. A {@link Mutex} gives a shared resource (a bus, a port) to one task
 * at a time.
 *
 * Stopping is cooperative: {@link Task.stop} sets {@link Task.stopped} and
 * ends at once the waits made by this module; the task leaves its loop and
 * ends its own way, then {@link Task.join} is resolved.
 *
 * @example
 * import { task } from "stdlib/tasks";
 * import { Gpio } from "device/gpio";
 *
 * const led = new Gpio(2, { mode: "output" });
 * const blink = task("blink", async self => {
 *     const tick = self.every(500);
 *     while (!self.stopped) {
 *         led.toggle();
 *         await tick.next();
 *     }
 *     led.write(0);
 * });
 *
 * // later
 * blink.stop();
 * await blink.join();
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | i.MX RT1062 | Twin |
 * |---|---|---|---|---|
 * | ✓ | ✓ | ✓ | ✓ | ✓ |
 *
 * @module stdlib/tasks
 */
declare module "stdlib/tasks" {
    /**
     * Starts a task: calls `body(self)` at once, as any async function
     * called (it runs until its first wait, then `task()` returns).
     *
     * An error of the body (thrown or rejected) is printed on the console as
     * `<name>: ended by an error: <error>` (the task is over: a loop that
     * must survive a failed step catches its errors itself), and
     * {@link Task.join} is rejected with it.
     *
     * @param name printed with the errors and the warnings of the task.
     * @param body the code of the task; `self` is the task itself.
     * @returns the task, to stop it and wait for its end.
     * @throws `TypeError` when `name` is not a string or `body` not a
     *     function.
     *
     * @example
     * const counter = task("counter", async self => {
     *     let count = 0;
     *     while (!self.stopped) {
     *         count++;
     *         await self.sleep(1000);
     *     }
     *     return count;
     * });
     *
     * counter.stop();
     * console.log(await counter.join());   // the seconds counted
     */
    export function task<T>(name: string, body: (self: Task<T>) => T | Promise<T>): Task<T>;

    /**
     * A task started by {@link task}.
     */
    export interface Task<T = unknown> {
        /** The name given to {@link task}. */
        readonly name: string;

        /**
         * True once {@link Task.stop} has been called: the task should leave
         * its loop and end. Tested by the task itself, usually as the
         * condition of its loop.
         */
        readonly stopped: boolean;

        /**
         * Asks the task to stop: {@link Task.stopped} becomes true, and the
         * waits of this task made by this module end at once
         * ({@link Task.sleep}, {@link Ticker.next}, the receiving of its
         * mailboxes). Other promises the task waits for (a send on a
         * strip, an I2C transfer) go on to their end.
         *
         * A task still not finished 5 s later is reported on the console
         * (`<name>: not finished 5 s after stop()`); {@link Task.join} still
         * waits for it. A second call does nothing.
         */
        stop(): void;

        /**
         * The end of the task.
         *
         * @returns a promise resolved with the value the body returned, or
         *     rejected with its error (printed by the task already).
         */
        join(): Promise<T>;

        /**
         * Waits `ms` milliseconds, or less if the task is stopped meanwhile
         * (at once if it is already).
         *
         * @throws `TypeError` when `ms` is not a number > 0.
         */
        sleep(ms: number): Promise<void>;

        /**
         * A ticker: steps every `period` milliseconds, at fixed times
         * counted from now (the time the loop takes does not shift them).
         * A step costs one promise and one timer; the ticker lives as long
         * as its task: make it once, before the loop.
         *
         * @throws `TypeError` when `period` is not a number > 0.
         *
         * @example
         * const tick = self.every(10);
         * while (!self.stopped) {
         *     draw();
         *     await tick.next();
         * }
         */
        every(period: number): Ticker;

        /**
         * A mailbox read by this task: values sent from anywhere (a
         * callback, another task), taken in order.
         *
         * @param size values it holds at most, an integer >= 1.
         * @throws `TypeError` when `size` is not an integer >= 1.
         *
         * @example
         * task("protocol", async self => {
         *     const inbox = self.mailbox(30);
         *     link.onLine(line => {
         *         if (!inbox.send(line)) {
         *             console.warn(`protocol: "${line}" dropped`);
         *         }
         *     });
         *     for await (const line of inbox) {
         *         await execute(line);
         *     }
         * });
         */
        mailbox<V>(size: number): Mailbox<V>;
    }

    /**
     * The steps of {@link Task.every}: at the start of the ticker + one
     * period, + two periods...
     */
    export interface Ticker {
        /**
         * Waits for the next step. When the step has passed already (the
         * loop took longer than a period), the wait ends at the next turn of
         * the event loop and the steps passed are skipped: a late loop does
         * not run several times in a row to catch up. Ends at once when the
         * task is stopped.
         */
        next(): Promise<void>;
    }

    /**
     * Values for a task, given by {@link Mailbox.send} and taken in order by
     * the task ({@link Mailbox.receive}, or `for await`). Once the task is
     * stopped, the values waiting are dropped and the receiving ends.
     */
    export interface Mailbox<V> extends AsyncIterable<V> {
        /**
         * Gives a value to the task; never waits.
         *
         * @returns false when the value is dropped: the mailbox is full, or
         *     the task stopped.
         */
        send(value: V): boolean;

        /**
         * The oldest value, waiting for one if there is none.
         *
         * @returns a promise of the value, or of `undefined` once the task
         *     is stopped.
         */
        receive(): Promise<V | undefined>;
    }

    /**
     * A resource shared by several tasks, used by one at a time: jobs given
     * to {@link Mutex.withLock} run one after the other, in the order of the
     * calls. Not reentrant: a job that calls `withLock` on its own mutex
     * waits for itself forever.
     *
     * @example
     * import { Mutex } from "stdlib/tasks";
     *
     * const bus = new Mutex();
     *
     * // nobody uses the bus between the two transfers
     * const count = await bus.withLock(async () => {
     *     await reader.selectAntenna(1);
     *     return await reader.tagCount();
     * });
     */
    export class Mutex {
        /**
         * Runs `job` once the jobs given before have ended.
         *
         * @returns a promise settled as the job's (its value, or its error);
         *     an error does not stop the jobs given after it.
         * @throws `TypeError` when `job` is not a function.
         */
        withLock<R>(job: () => R | Promise<R>): Promise<R>;
    }
}
