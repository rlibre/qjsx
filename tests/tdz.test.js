/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The temporal dead zone of let / const / class: every access before the
// declaration has run must still throw a ReferenceError, whatever the path
// that leads to it (blocks, switch, loops, labels, try / catch / finally,
// generators, closures), and every access after it must read the right
// value. Guards the compiler pass that removes the checks it can prove
// useless (engine change N, compiler/tdz.c).
// Run with: bin/Release/qjsx run tests/tdz.test.js
import { check, equal, throws, report } from "./assert.js";

const TDZ = /^ReferenceError: /;

// ### Accesses before the declaration: they throw #############################

throws(() => {
    x;
    let x = 1;
}, TDZ, "read before the declaration");

throws(() => {
    x = 2;
    let x;
}, TDZ, "write before the declaration");

throws(() => {
    let x = x + 1;
}, TDZ, "read in its own initializer");

throws(() => {
    return typeof x;
    let x;
}, TDZ, "typeof before the declaration");

throws(() => {
    c;
    const c = 1;
}, TDZ, "const read before the declaration");

throws(() => {
    new C();
    class C {}
}, TDZ, "class used before the declaration");

throws(() => {
    let { a = b, b } = {};
}, TDZ, "destructuring default reading a later binding");

throws(() => {
    let [p = q, q] = [];
}, TDZ, "array destructuring default reading a later binding");

// switch: a case reached without running the declaration of another one
throws(() => {
    switch (1) {
        case 0:
            let s = 1;
            break;
        case 1:
            s;
    }
}, TDZ, "switch: read of a declaration of another case");

throws(() => {
    switch (1) {
        case 0:
            let s = 1;
            break;
        case 1:
            s = 5;
    }
}, TDZ, "switch: write of a declaration of another case");

// loops: each turn enters the block again, its declarations are fresh
throws(() => {
    for (let n = 0; n < 2; n++) {
        if (n === 1) {
            y;
        }

        let y = n;
    }
}, TDZ, "for: read before the declaration on the second turn");

throws(() => {
    let i = 0;
    while (i < 2) {
        if (i === 1) {
            z += 1;
        }

        let z = i;
        i++;
    }
}, TDZ, "while: write before the declaration on the second turn");

throws(() => {
    outer: for (let k = 0; k < 2; k++) {
        if (k === 1) {
            w;
        }

        if (k === 0) {
            continue outer;
        }

        let w = 1;
    }
}, TDZ, "labelled continue skipping the declaration");

throws(() => {
    let turn = 0;
    do {
        if (turn === 1) {
            v;
        }

        let v = turn;
        turn++;
    } while (turn < 2);
}, TDZ, "do while: read before the declaration on the second turn");

// try / catch / finally
throws(() => {
    try {
        throw new Error("first");
    } catch (error) {
        q;
    }

    let q = 1;
}, TDZ, "catch: read of a later declaration");

throws(() => {
    try {
        r;
    } finally {
        // nothing
    }

    let r = 1;
}, TDZ, "try with finally: read of a later declaration");

// closures and hoisted functions read through the closure (not touched by
// the pass), still checked
throws(() => {
    const read = () => m;
    read();
    let m = 1;
}, TDZ, "closure called before the declaration");

throws(() => {
    hoisted();
    let h = 1;
    function hoisted() {
        return h;
    }
}, TDZ, "hoisted function called before the declaration");

// generators: the declaration after a yield
throws(() => {
    function* generator() {
        yield 1;
        t;
        let t = 2;
    }

    const iterator = generator();
    iterator.next();
    iterator.next();
}, TDZ, "generator: read before the declaration after a yield");

// ### Accesses after the declaration: they read the right value ###############

{
    let total = 0;
    for (let i = 0; i < 10; i++) {
        total += i;
    }

    equal(total, 45, "for: let counter and accumulator");
}

{
    const functions = [];
    for (let i = 0; i < 3; i++) {
        functions.push(() => i);
    }

    equal(functions.map(f => f()).join(","), "0,1,2", "for: one binding per turn, seen by closures");
}

{
    let found = -1;
    switch (2) {
        case 1:
            found = 1;
            break;
        case 2: {
            let local = 20;
            local += 2;
            found = local;
            break;
        }
    }

    equal(found, 22, "switch: a declaration read in its own case");
}

{
    let before = 5;
    let seen = 0;
    try {
        before += 1;
        throw new Error("x");
    } catch {
        seen = before;
    } finally {
        before += 10;
    }

    equal(seen, 6, "catch: a variable initialized before the try");
    equal(before, 16, "finally: a variable initialized before the try");
}

{
    function* generator() {
        let count = 0;
        while (count < 3) {
            yield count;
            count++;
        }
    }

    equal([...generator()].join(","), "0,1,2", "generator: let across yields");
}

{
    let shadow = 1;
    {
        let shadow = 2;
        shadow += 1;
        equal(shadow, 3, "inner binding");
    }

    equal(shadow, 1, "outer binding untouched");
}

{
    let label = "";
    outer: for (let a = 0; a < 3; a++) {
        for (let b = 0; b < 3; b++) {
            if (b === 1) {
                continue outer;
            }

            if (a === 2) {
                break outer;
            }

            label += `${a}${b} `;
        }
    }

    equal(label, "00 10 ", "labelled break and continue");
}

{
    const limit = 4;
    let product = 1;
    let index = 1;
    do {
        product *= index;
        index++;
    } while (index <= limit);

    equal(product, 24, "do while: const and let");
}

report("tdz");
