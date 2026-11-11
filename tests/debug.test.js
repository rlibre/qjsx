// Tests of the debugger support of the engine (quickjs-debug.h): stops,
// stepping, 'debugger;', exceptions, frames and slots, checked against the
// debug info (sidecar) given by the compiler.
// Run with: bin/Release/qjsx-tests run tests/debug.test.js
import * as fs from "fs";
import { compile } from "compiler";
import * as debug from "debug_test";
import * as target from "./fixtures/debug/target.js";
import { check, equal, report } from "./assert.js";

const TARGET_PATH = "tests/fixtures/debug/target.js";
const POSITION_STATEMENT = 1;
const POSITION_RETURN = 2;

// Compares values that JSON can represent (arrays, plain objects).
function equalJson(actual, expected, message) {
    equal(JSON.stringify(actual), JSON.stringify(expected), message);
}

// --- debug info of the target, compiled like the package builder does
const { module } = debug.functionLocation(target.add);
check(module.endsWith("target.js"), "module name of a function");
const source = fs.readFileSync(TARGET_PATH, "utf8");
const sidecar = compile(source, module, { module: true, strip: "all", debug: true }).debug;

function functionInfo(fn) {
    const info = sidecar[debug.functionLocation(fn).number];
    check(info !== undefined, `sidecar has the function ${fn.name}`);
    return info;
}

// Positions of a function as objects { pc, offset, kind, text }, text being
// the source from the position to the end of its line.
function positions(info) {
    const result = [];
    for (let i = 0; i < info.positions.length; i += 3) {
        const offset = info.positions[i + 1];
        const lineEnd = source.indexOf("\n", offset);
        result.push({
            pc: info.positions[i],
            offset,
            kind: info.positions[i + 2],
            text: source.slice(offset, lineEnd < 0 ? undefined : lineEnd).trim(),
        });
    }
    return result;
}

function stops(info) {
    return positions(info).filter(position => position.kind !== 0);
}

// The stop position at 'pc' (what the proxy shows for a pause).
function stopAt(info, pc) {
    return stops(info).find(position => position.pc === pc);
}

function localIndex(info, name) {
    return info.locals.findIndex(variable => variable.name === name);
}

// --- breakpoint, frames and slots
const addInfo = functionInfo(target.add);
const addStops = stops(addInfo);
equalJson(addStops.map(stop => stop.text),
    ["const sum = a + b;", "let doubled = sum * 2;", "return doubled;", "return doubled;"],
    "statement and return positions of add()");
equal(addStops[3].kind, POSITION_RETURN, "the last stop of add() is its return");

const addLocation = debug.functionLocation(target.add);
const doubledStop = addStops[1];
let events = [];
debug.setHook(event => {
    const frame0 = debug.frame(0);
    const frame1 = debug.frame(1);
    events.push({
        reason: event.reason,
        frame: frame0,
        caller: frame1,
        a: debug.slot(0, "argument", 0),
        sum: debug.slot(0, "local", localIndex(addInfo, "sum")),
        doubled: debug.slot(0, "local", localIndex(addInfo, "doubled")),
    });
    // the debugger changes a variable
    check(debug.setSlot(0, "local", localIndex(addInfo, "sum"), 100), "setSlot");
});
check(debug.setStops(addLocation.module, addLocation.number, "breakpoints", [doubledStop.pc]), "setStops");
check(!debug.setStops(addLocation.module, 9999, "breakpoints", [0]), "setStops of an unknown function fails");
check(!debug.setStops("unknown.js", 0, "breakpoints", [0]), "setStops of an unknown module fails");

equal(target.add(2, 3), 200, "the breakpoint changed 'sum' before 'doubled' was computed");
equal(events.length, 1, "one breakpoint hit");
equal(events[0].reason, "breakpoint", "reason");
equal(events[0].frame.number, addLocation.number, "frame 0 is add()");
equal(events[0].frame.pc, doubledStop.pc, "frame 0 stops at the breakpoint pc");
equal(events[0].a, 2, "argument a");
equal(events[0].sum, 5, "local sum");
equal(events[0].doubled, debug.UNINITIALIZED, "'doubled' is in its temporal dead zone");
check(events[0].caller.isBytecode && events[0].caller.module.endsWith("debug.test.js"), "frame 1 is the test module");
check(debug.frame(0) === null, "no frame outside the hook");

// --- stop on return: the returned value is given
events = [];
debug.setHook(event => events.push({ reason: event.reason, atReturn: event.atReturn, value: event.returnValue }));
debug.setStops(addLocation.module, addLocation.number, "breakpoints", [addStops[3].pc]);
equal(target.add(1, 1), 4, "add() with a breakpoint on its return");
equalJson(events, [{ reason: "breakpoint", atReturn: true, value: 4 }], "return value at the return stop");
debug.setStops(addLocation.module, addLocation.number, "breakpoints", []);
events = [];
target.add(1, 1);
equal(events.length, 0, "breakpoints removed");

// --- stepping: every statement start of loop()
const loopInfo = functionInfo(target.loop);
const loopLocation = debug.functionLocation(target.loop);
debug.setStops(loopLocation.module, loopLocation.number, "statements", stops(loopInfo).map(stop => stop.pc));
const steps = [];
debug.setHook(event => {
    const stop = stopAt(loopInfo, debug.frame(0).pc);
    steps.push(`${event.reason}:${stop ? stop.text : "?"}`);
});
target.loop(2);
equal(steps.length, 0, "no step stop while stepping is off");
debug.setStepping(true);
equal(target.loop(2), 1, "loop() result while stepping");
debug.setStepping(false);
equalJson(steps, [
    "step:let total = 0;",
    "step:for (let i = 0; i < n; i++) {",     // the loop statement
    "step:i < n; i++) {",                     // test
    "step:total += i;",
    "step:i++) {",                            // update
    "step:i < n; i++) {",                     // test
    "step:total += i;",
    "step:i++) {",                            // update
    "step:i < n; i++) {",                     // test, false
    "step:return total;",
    "step:return total;",                     // the return instruction
], "statements met while stepping through loop()");

// --- 'debugger;' statements
const tdzInfo = functionInfo(target.tdz);
const tdzNumber = debug.functionLocation(target.tdz).number;
events = [];
debug.setHook(event => {
    const inTdz = debug.frame(0).number === tdzNumber;
    const later = inTdz ? debug.slot(0, "local", localIndex(tdzInfo, "later")) : null;
    events.push({ reason: event.reason, later });
});
equal(target.withDebugger(1), 2, "withDebugger()");
equal(target.tdz(), 1, "tdz()");
equalJson(events.map(event => event.reason), ["statement", "statement"], "debugger statements");
equal(events[1].later, debug.UNINITIALIZED, "'later' before its declaration");

// --- exceptions: reported once, in the frame of the throw
const throwerNumber = debug.functionLocation(target.thrower).number;
events = [];
debug.setHook(event => events.push({
    reason: event.reason,
    message: event.exception instanceof Error ? event.exception.message : String(event.exception),
    number: debug.frame(0).number,
}));
target.catcher();
equal(events.length, 0, "no exception reported while reporting is off");
debug.setExceptionReporting(true);
equal(target.catcher(), "caught", "catcher()");
equalJson(events, [{ reason: "exception", message: "boom", number: throwerNumber }], "caught exception reported at the throw");
events = [];
let escaped = null;
try {
    target.throughFinally();
} catch (error) {
    escaped = error.message;
}
equal(escaped, "boom", "the exception went through the finally clause");
equal(events.length, 1, "an exception rethrown by 'finally' is reported once");
events = [];
try {
    target.throughFinallyWithCode();
} catch (error) {
    escaped = error.message;
}
equal(target.cleanups, 1, "the finally clause ran");
equal(events.length, 1, "an exception rethrown by a non-empty 'finally' is reported once");
events = [];
equal(await target.asyncCatcher(), "async caught", "asyncCatcher()");
equalJson(events.map(event => event.message), ["async boom"], "an exception crossing an 'await' is reported once");
debug.setExceptionReporting(false);

// --- no hook: nothing is called
debug.setHook(null);
equal(target.withDebugger(1), 2, "debugger statement without hook");
equal(debug.hookErrorCount(), 0, "no error in the hook callbacks");

report("debug");
