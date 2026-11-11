/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tests of the debugger stub (common/agent/debugger.*) through the desktop
// agent: builds tests/fixtures/debuggee, installs it, debugs it with the
// agent protocol (no CDP: that is the proxy's job).
// Run with: bin/Release/qjsx run tests/debugger.test.js tcp:127.0.0.1:<port>
// (an "qjsx agent" must listen there; tests/run-tests.sh starts one).
import * as fs from "fs";
import { compile } from "compiler";
import { build } from "qjsx:build";
import { readZip } from "qjsx:zip";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import {
    KIND, KEY, VALUE, OBJECT_KIND, STEP_KIND, STOP_KIND, EXCEPTION_MODE, SLOT_KIND, GROUP,
    PAUSE_REASON, encodeStops,
} from "qjsx:protocol";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const PROJECT = "tests/fixtures/debuggee";
const EVENT_TIMEOUT = 5000;
const POSITION_STATEMENT = 1;

// --- events of the device, awaited in order
const events = [];
const waiters = [];
const output = [];

function onEvent(kind, payload) {
    const waiter = waiters.shift();
    if (waiter !== undefined) {
        clearTimeout(waiter.timer);
        waiter.resolve({ kind, payload });
    } else {
        events.push({ kind, payload });
    }
}

function nextEvent() {
    if (events.length > 0) {
        return Promise.resolve(events.shift());
    }
    return new Promise((resolve, reject) => {
        const waiter = { resolve, timer: undefined };
        waiter.timer = setTimeout(() => {
            waiters.splice(waiters.indexOf(waiter), 1);
            reject(new Error("no event from the device"));
        }, EVENT_TIMEOUT);
        waiters.push(waiter);
    });
}

async function expectEvent(kind, message) {
    const event = await nextEvent();
    equal(event.kind, kind, message);
    return event.payload;
}

// Waits for the STARTED of the run the test asked for. A program may already
// run on the device when the test connects (the installed package starts at
// boot): the STOP of the test ends it, and its EXITED arrives later, but
// always before our STARTED (the device ends a run before it starts the
// next one). That EXITED is skipped; any other event fails the check.
async function expectStarted(message) {
    let event = await nextEvent();
    while (event.kind === KIND.EXITED) {
        event = await nextEvent();
    }

    equal(event.kind, KIND.STARTED, message);
    return event.payload;
}

// --- debug info of the package
function readPackage(path) {
    const entries = readZip(fs.readFileSync(path));
    const decode = name => JSON.parse(new TextDecoder().decode(entries.get(name)));
    const manifest = decode("manifest.json");
    const modules = new Map();
    for (const script of manifest.scripts) {
        const source = new TextDecoder().decode(entries.get(script.source));
        modules.set(script.path, { source, functions: decode(script.debug).functions });
    }
    return modules;
}

function stopPositions(info) {
    const result = [];
    for (let i = 0; i < info.positions.length; i += 3) {
        if (info.positions[i + 2] !== 0) {
            result.push({ pc: info.positions[i], offset: info.positions[i + 1] });
        }
    }
    return result;
}

function functionNamed(module, name) {
    return module.functions.find(info => info.name === name);
}

// pc of the statement of 'info' whose source starts with 'text'.
function statementPc(module, info, text) {
    const stop = stopPositions(info).find(position => module.source.startsWith(text, position.offset));
    return stop?.pc;
}

// Text of the statement at 'pc' (start of its line from the position).
function statementAt(module, info, pc) {
    const stop = stopPositions(info).find(position => position.pc === pc);
    if (stop === undefined) {
        return undefined;
    }
    const lineEnd = module.source.indexOf("\n", stop.offset);
    return module.source.slice(stop.offset, lineEnd).trim();
}

function isObjectValue(value, kind) {
    return Array.isArray(value) && value[0] === VALUE.OBJECT && value[2] === kind;
}

// --- the test
const projectBuild = build(PROJECT);
const modules = readPackage(projectBuild.file);
const main = modules.get("src/main.js");
const compute = functionNamed(main, "compute");
const spin = functionNamed(main, "spin");
check(compute !== undefined && spin !== undefined, "sidecar has the functions of main.js");

const device = await Device.open(TARGET, {
    onEvent,
    onLog: (stream, text) => output.push(text),
});
await device.hello();
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(projectBuild.file));
await device.request(KIND.DEBUG_ATTACH);
await device.request(KIND.SET_EXCEPTIONS, { [KEY.MODE]: EXCEPTION_MODE.ALL });

// Each loaded module: statements of all its functions (pause and steps),
// a breakpoint in compute(), then RESUME.
const breakpointPc = statementPc(main, compute, "const result = area(box);");
check(breakpointPc !== undefined, "breakpoint position found in the sidecar");

async function onScriptLoaded(payload) {
    const name = payload.get(KEY.MODULE);
    const module = modules.get(name);
    check(module !== undefined, `loaded module ${name} is in the package`);
    const statements = module.functions.map(info => ({ number: info.number, offsets: stopPositions(info).map(p => p.pc) }));
    const answer = await device.request(KIND.SET_STOPS, {
        [KEY.MODULE]: name, [KEY.STOP_KIND]: STOP_KIND.STATEMENTS, [KEY.DATA]: encodeStops(statements),
    });
    equal(answer.get(KEY.MISSING), 0, `stops of every function of ${name} set`);
    if (name === "src/main.js") {
        await device.request(KIND.SET_STOPS, {
            [KEY.MODULE]: name, [KEY.STOP_KIND]: STOP_KIND.BREAKPOINTS,
            [KEY.DATA]: encodeStops([{ number: compute.number, offsets: [breakpointPc] }]),
        });
    }
    await device.request(KIND.RESUME);
}

await device.request(KIND.RUN);
await expectStarted("program started");
await onScriptLoaded(await expectEvent(KIND.SCRIPT_LOADED, "main module loaded"));
await onScriptLoaded(await expectEvent(KIND.SCRIPT_LOADED, "imported module loaded"));
const scripts = await device.request(KIND.LIST_SCRIPTS);
equal(scripts.get(KEY.SCRIPTS).join(","), "src/main.js,src/shapes.js", "LIST_SCRIPTS");

// --- breakpoint: stack, slots, properties, evaluation
let paused = await expectEvent(KIND.PAUSED, "paused at the breakpoint");
equal(paused.get(KEY.REASON), PAUSE_REASON.BREAKPOINT, "reason: breakpoint");
let frames = (await device.request(KIND.GET_STACK)).get(KEY.FRAMES);
const [, frameModule, frameNumber, framePc] = frames[0];
equal(frameModule, "src/main.js", "frame 0 module");
equal(frameNumber, compute.number, "frame 0 is compute()");
equal(framePc, breakpointPc, "frame 0 pc is the breakpoint");
equal(frames[1][2], functionNamed(main, "<eval>").number, "frame 1 is the module code");

const argumentsValues = (await device.request(KIND.GET_SLOTS, { [KEY.LEVEL]: 0, [KEY.SLOT_KIND]: SLOT_KIND.ARGUMENT })).get(KEY.VALUES);
equal(argumentsValues.join(","), "3,4", "arguments width, height");
const locals = (await device.request(KIND.GET_SLOTS, { [KEY.LEVEL]: 0, [KEY.SLOT_KIND]: SLOT_KIND.LOCAL })).get(KEY.VALUES);
const boxIndex = compute.locals.findIndex(variable => variable.name === "box");
const resultIndex = compute.locals.findIndex(variable => variable.name === "result");
const box = locals[boxIndex];
check(isObjectValue(box, OBJECT_KIND.PLAIN), "'box' is an object");
equal(box[3], "Object", "class name of 'box'");
check(Array.isArray(locals[resultIndex]) && locals[resultIndex][0] === VALUE.UNINITIALIZED, "'result' in its temporal dead zone");

const properties = (await device.request(KIND.GET_PROPERTIES, { [KEY.HANDLE]: box[1] }));
const names = properties.get(KEY.PROPERTIES).map(property => property[0]);
equal(names.join(","), "width,height,label", "own properties of 'box'");
equal(properties.get(KEY.PROPERTIES)[2][2], "box", "value of 'label'");
equal(properties.get(KEY.INTERNALS)[0][0], "[[Prototype]]", "internal [[Prototype]]");

const helperSource = "(function (shape, factor) { return shape.width * factor; })";
const helper = compile(helperSource, "<debugger>", { module: false, strip: "all" }).bytecode;
const loaded = (await device.request(KIND.LOAD_FUNCTION, { [KEY.DATA]: helper, [KEY.GROUP]: GROUP.PERSISTENT })).get(KEY.VALUE);
check(isObjectValue(loaded, OBJECT_KIND.FUNCTION), "LOAD_FUNCTION gives a function");
const called = await device.request(KIND.CALL, {
    [KEY.FUNCTION]: [VALUE.HANDLE, loaded[1]],
    [KEY.THIS]: undefined,
    [KEY.ARGUMENTS]: [[VALUE.HANDLE, box[1]], 10],
});
equal(called.get(KEY.VALUE), 30, "CALL of a host-compiled function with a handle argument");
const failed = await device.request(KIND.CALL, { [KEY.FUNCTION]: [VALUE.HANDLE, loaded[1]], [KEY.ARGUMENTS]: [] });
check(isObjectValue(failed.get(KEY.EXCEPTION), OBJECT_KIND.ERROR), "CALL that throws gives the exception");

// change an argument
await device.request(KIND.SET_SLOT, { [KEY.LEVEL]: 0, [KEY.SLOT_KIND]: SLOT_KIND.ARGUMENT, [KEY.INDEX]: 0, [KEY.VALUE]: 5 });
const changedWidth = (await device.request(KIND.GET_SLOTS, { [KEY.LEVEL]: 0, [KEY.SLOT_KIND]: SLOT_KIND.ARGUMENT })).get(KEY.VALUES)[0];
equal(changedWidth, 5, "SET_SLOT changed an argument");
// replace 'box' by an object made on the device: area() then gives 5 * 4
const makeBoxSource = "(function (width, height) { return { width, height }; })";
const makeBox = compile(makeBoxSource, "<debugger>", { module: false, strip: "all" }).bytecode;
const makeBoxFunction = (await device.request(KIND.LOAD_FUNCTION, { [KEY.DATA]: makeBox })).get(KEY.VALUE);
const newBox = (await device.request(KIND.CALL, { [KEY.FUNCTION]: [VALUE.HANDLE, makeBoxFunction[1]], [KEY.ARGUMENTS]: [5, 4] })).get(KEY.VALUE);
await device.request(KIND.SET_SLOT, {
    [KEY.LEVEL]: 0, [KEY.SLOT_KIND]: SLOT_KIND.LOCAL, [KEY.INDEX]: boxIndex, [KEY.VALUE]: [VALUE.HANDLE, newBox[1]],
});

// --- steps
async function stepAndWhere(kind) {
    await device.request(KIND.STEP, { [KEY.STEP]: kind });
    await expectEvent(KIND.RESUMED, "resumed for the step");
    const pausedAgain = await expectEvent(KIND.PAUSED, "paused after the step");
    const top = (await device.request(KIND.GET_STACK)).get(KEY.FRAMES)[0];
    return { reason: pausedAgain.get(KEY.REASON), module: top[1], number: top[2], pc: top[3], paused: pausedAgain };
}

const shapes = modules.get("src/shapes.js");
const areaInfo = functionNamed(shapes, "area");
let where = await stepAndWhere(STEP_KIND.INTO);
equal(where.reason, PAUSE_REASON.STEP, "reason: step");
equal(`${where.module}:${statementAt(shapes, areaInfo, where.pc)}`, "src/shapes.js:const surface = shape.width * shape.height;", "step into area()");
where = await stepAndWhere(STEP_KIND.OUT);
equal(`${where.module}:${statementAt(main, compute, where.pc)}`, "src/main.js:return result;", "step out of area()");
where = await stepAndWhere(STEP_KIND.OVER);
equal(where.paused.get(KEY.RETURN_VALUE), 20, "step over to the return of compute(): value 5 * 4");

// --- pause of a running loop, then end it by changing a variable
await device.request(KIND.RESUME);
await expectEvent(KIND.RESUMED, "resumed");
await new Promise(resolve => setTimeout(resolve, 200));
await device.request(KIND.PAUSE);
paused = await expectEvent(KIND.PAUSED, "paused in the running loop");
frames = (await device.request(KIND.GET_STACK)).get(KEY.FRAMES);
equal(frames[0][2], spin.number, "paused in spin()");
const flagIndex = spin.closures.findIndex(variable => variable.name === "keepSpinning");
await device.request(KIND.SET_SLOT, { [KEY.LEVEL]: 0, [KEY.SLOT_KIND]: SLOT_KIND.CLOSURE, [KEY.INDEX]: flagIndex, [KEY.VALUE]: false });
await device.request(KIND.RESUME);
await expectEvent(KIND.RESUMED, "resumed after changing the loop flag");

// --- exception, then 'debugger;'
paused = await expectEvent(KIND.PAUSED, "paused on the exception");
equal(paused.get(KEY.REASON), PAUSE_REASON.EXCEPTION, "reason: exception");
check(isObjectValue(paused.get(KEY.EXCEPTION), OBJECT_KIND.ERROR), "the exception is an Error");
check(paused.get(KEY.EXCEPTION)[4].startsWith("Error: expected failure"), "description of the exception");
await device.request(KIND.RESUME);
await expectEvent(KIND.RESUMED, "resumed after the exception");
paused = await expectEvent(KIND.PAUSED, "paused on 'debugger;'");
equal(paused.get(KEY.REASON), PAUSE_REASON.DEBUGGER, "reason: debugger statement");
await device.request(KIND.RESUME);
await expectEvent(KIND.RESUMED, "resumed after 'debugger;'");

// --- step over the end of a function: not into the next call at the same
// place of the C stack (second() after first()), but to the next statement
// of the caller
const moduleCode = functionNamed(main, "<eval>");
const firstInfo = functionNamed(main, "first");
await expectEvent(KIND.PAUSED, "paused on 'debugger;' in first()");
where = { number: firstInfo.number };
for (let i = 0; i < 5 && where.number === firstInfo.number; i++) {
    where = await stepAndWhere(STEP_KIND.OVER);
}
equal(`${where.number}:${statementAt(main, moduleCode, where.pc)}`, `${moduleCode.number}:waitAndCount().then(showTicks);`,
    "step over the end of first(): next statement of the module, not second()");
await device.request(KIND.RESUME);
await expectEvent(KIND.RESUMED, "resumed after the steps over first()");

// --- step over an 'await' while a timer fires, then out of the async
// function resumed by a promise job
const waitInfo = functionNamed(main, "waitAndCount");
const showInfo = functionNamed(main, "showTicks");
await expectEvent(KIND.PAUSED, "paused on 'debugger;' in waitAndCount()");
where = await stepAndWhere(STEP_KIND.OVER);
equal(`${where.number}:${statementAt(main, waitInfo, where.pc)}`, `${waitInfo.number}:await sleep(20);`, "step over to the 'await'");
where = await stepAndWhere(STEP_KIND.OVER);
equal(`${where.number}:${statementAt(main, waitInfo, where.pc)}`, `${waitInfo.number}:return ticks;`,
    "step over the 'await': back in waitAndCount(), not in the timer");
where = await stepAndWhere(STEP_KIND.OUT);
equal(`${where.number}:${statementAt(main, showInfo, where.pc)}`, `${showInfo.number}:console.log("ticks", count);`,
    "step out of the resumed async function: the next code run");
await device.request(KIND.RESUME);
await expectEvent(KIND.RESUMED, "resumed after the async steps");

const exited = await expectEvent(KIND.EXITED, "program ended");
equal(exited.get(KEY.CODE), 0, "exit code");
equal(output.join(""), "area 20\nspun true\nexpected failure\nsum 3\nticks 1\ndone\n", "program output");

await device.request(KIND.DEBUG_DETACH);
device.close();
fs.rmSync(`${PROJECT}/build`, { recursive: true, force: true });
report("debugger");
