/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tests of the binding library through the native module "jsbind_test".
// Run with: bin/Release/qjsx-tests run tests/jsbind.test.js
import { Device, add, gc, destroyedCount } from "jsbind_test";
import { check, equal, throws, rejects, report } from "./assert.js";

// --- free functions and argument checks
equal(add(2, 3), 5, "add");
throws(() => add(2, "3"), /^TypeError: jsbind_test\.add: argument 2: must be an integer/, "add with a string");
throws(() => add(2.5, 1), /must be an integer/, "add with a fraction");

// --- construction through init(), with a constrained argument
const device = new Device(1);
equal(device.portNumber, 1, "init sets the port");
throws(() => new Device(3), /^RangeError: Device: argument 1: must be between 0 and 1/, "port out of range");
throws(() => Device(0), /TypeError/, "constructor without new");
check(device instanceof Device, "instanceof");

// --- properties: read-only, writable with conversion
equal(device.name, "device", "read-only property");
throws(() => { device.name = "other"; }, /^TypeError: no setter/, "assignment to a read-only property (strict mode)");
equal(device.speed, 0, "initial speed");
device.speed = 9600;
equal(device.speed, 9600, "property setter");
throws(() => { device.speed = "fast"; }, /Device.speed: must be an integer/, "setter conversion");

// --- structs and enums
throws(() => device.readChar(), /^Error: port not open$/, "js::fail in a method returning int");
device.open({ speed: 9600, parity: "even" });
equal(device.speed, 9600, "struct field");
equal(device.parity, "even", "enum as string");
equal(device.readChar(), 65, "readChar");
throws(() => device.open({}), /already open/, "state error");
device.close();
throws(() => device.open({ speed: 9600, parrity: "odd" }), /unknown field 'parrity'/, "unknown field");
throws(() => device.open({ parity: "mark" }), /field 'parity': must be one of 'none', 'odd', 'even'/, "bad enum");
throws(() => device.open({ speed: 10 }), /^RangeError: .*field 'speed': must be between 300 and 4000000/, "range in struct");
device.open({});
const options = device.options();
equal(options.speed, 115200, "default value of a struct field");
equal(options.parity, "none", "struct returned to JS");
device.close();

// --- this checks
throws(() => Device.prototype.readChar.call({}), /TypeError/, "method on a wrong object");

// --- first error wins
throws(() => device.twoErrors(), /^TypeError: first$/, "first error wins");

// --- system errors
const systemError = throws(() => device.systemError(), /^Error: ENOENT/, "failSystem");
equal(systemError && systemError.code, "ENOENT", "error.code");

// --- variadic arguments, bytes, class pointers
equal(device.sum(1, 2, 3.5), 6.5, "js::Args");
equal(device.byteCount(new Uint8Array(7)), 7, "bytes from Uint8Array");
equal(device.byteCount(new ArrayBuffer(5)), 5, "bytes from ArrayBuffer");
equal(device.byteCount(new Uint16Array(4).subarray(1)), 6, "bytes from a typed array view");
const bytes = device.makeBytes(4);
check(bytes instanceof Uint8Array && bytes.length === 4 && bytes[3] === 3, "js::Buffer returned");
check(device.isOther(new Device(0)), "class pointer argument");
check(!device.isOther(device), "same object");
throws(() => device.isOther({}), /must be a Device/, "wrong class argument");
check(device.self() === device, "class pointer returned to JS is the same object");

// --- callbacks: exceptions propagate through C++
let received = "";
device.onData(text => { received = text; });
device.emit("hello");
equal(received, "hello", "callback called with a converted argument");
device.onData(() => { throw new SyntaxError("from callback"); });
throws(() => device.emit("x"), /^SyntaxError: from callback$/, "exception from a callback");

// --- promises
const later = device.readLater();
check(device.isPending(), "promise pending");
device.resolvePending("data");
equal(await later, "data", "promise resolved");
check(!device.isPending(), "resolver cleared");
const failing = device.readLater();
device.rejectPending("bad");
await rejects(failing, /^RangeError: bad$/, "promise rejected");

// --- garbage collection: a cycle device -> callback -> device is collected
// only if the callback held by C++ is reported to the GC (mark function).
const before = destroyedCount();
(function makeCycle() {
    const cyclic = new Device(0);
    cyclic.onData(() => cyclic.readChar());
})();
gc();
check(destroyedCount() > before, "cycle through a C++-held callback is collected");

// --- keepAlive: an open device without JS reference keeps the loop running
(function openAndForget() {
    const kept = new Device(0);
    kept.open({});
    kept.onData(() => { kept.close(); });
    setTimeout(() => kept.emit("stop"), 10);
})();
gc();

setTimeout(() => {
    report("jsbind");
}, 50);
