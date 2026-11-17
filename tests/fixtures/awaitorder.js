/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The order in which awaits resume, against .then() reactions counting the
// turns of the job queue, in every case engine change V (doc/engine-changes.md)
// handles: await of values, of native promises (settled, pending, rejected),
// of a subclass and of a promise whose 'constructor' is read through a getter,
// of thenables; await f() of an async function that returns at once, returns
// a promise or a thenable, throws at once or later, over three levels. The
// output must be the same as upstream QuickJS's (tests/run-tests.sh compares
// it with tests/fixtures/awaitorder.expected, written by qjs-original).
// Plain script: no import, console.log only (runs on upstream qjs too).
const log = [];
const note = text => log.push(text);

// a clock of the job queue: one note per turn, for 'turns' turns
function ticks(name, turns) {
    let p = Promise.resolve();
    for (let i = 1; i <= turns; i++) {
        p = p.then(() => note(`${name} tick ${i}`));
    }
    return p;
}

async function returnsAtOnce(x) { return x; }
async function returnsPromise(x) { return Promise.resolve(x); }
async function returnsThenable(x) { return { then(resolve) { note("thenable then called"); resolve(x); } }; }
async function throwsAtOnce() { throw new Error("at once"); }
async function throwsLater() { await null; throw new Error("later"); }
async function level3(x) { await null; return x * 2; }
async function level2(x) { return await level3(x); }
async function level1(x) { return await level2(x); }

class SubPromise extends Promise {}

async function scenario(name, body) {
    note(`${name} start`);
    try {
        const value = await body();
        note(`${name} resumed with ${value}`);
    } catch (error) {
        note(`${name} threw ${error.message}`);
    }
}

async function main() {
    const watched = Promise.resolve("watched");
    Object.defineProperty(watched, "constructor", {
        get() { note("constructor read"); return Promise; }
    });
    let release;
    const pending = new Promise(resolve => { release = resolve; });

    const all = [
        ticks("A", 8),
        scenario("value", () => 5),
        scenario("settled", () => Promise.resolve("settled")),
        scenario("rejected", () => Promise.reject(new Error("rejected"))),
        scenario("pending", () => pending),
        scenario("subclass", () => SubPromise.resolve("subclass")),
        scenario("getter", () => watched),
        scenario("thenable", () => ({ then(resolve) { resolve("thenable"); } })),
        ticks("B", 8),
    ];
    Promise.resolve().then(() => { note("release"); release("pending"); });

    // await f() directly, in a function of its own each
    all.push((async () => { note("f() start"); note(`f() ${await returnsAtOnce(1)}`); })());
    all.push((async () => { note("promise() start"); note(`promise() ${await returnsPromise(2)}`); })());
    all.push((async () => { note("thenable() start"); note(`thenable() ${await returnsThenable(3)}`); })());
    all.push((async () => {
        note("throwsAtOnce() start");
        try { await throwsAtOnce(); } catch (error) { note(`throwsAtOnce() ${error.message}`); }
    })());
    all.push((async () => {
        note("throwsLater() start");
        try { await throwsLater(); } catch (error) { note(`throwsLater() ${error.message}`); }
    })());
    all.push((async () => { note("levels start"); note(`levels ${await level1(21)}`); })());
    all.push((async () => {
        const obj = { async method(x) { return x + 1; } };
        note("method start");
        note(`method ${await obj.method(9)}`);
    })());
    all.push(ticks("C", 10));

    await Promise.all(all);
    for (const line of log) {
        console.log(line);
    }
}

main();
