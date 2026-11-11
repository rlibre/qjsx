/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tests of the core of the debugger proxy (qjsx:debugsession, qjsx:debuginfo)
// through the desktop agent: builds tests/fixtures/debuggee, installs it and
// debugs it by source lines and variable names, as the CDP server will.
// Run with: bin/Release/qjsx run tests/proxy.test.js tcp:127.0.0.1:<port>
// (an "qjsx agent" must listen there; tests/run-tests.sh starts one). Also
// runs on a board: serial:COM11@921600.
import * as fs from "fs";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND } from "qjsx:protocol";
import { PackageIndex } from "qjsx:debuginfo";
import { DebugSession } from "qjsx:debugsession";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const PROJECT = "tests/fixtures/debuggee";
const EVENT_TIMEOUT = 5000;

// --- what the session reports, awaited in order
function eventQueue(what) {
    const items = [];
    const waiters = [];
    return {
        push(item) {
            const waiter = waiters.shift();
            if (waiter !== undefined) {
                clearTimeout(waiter.timer);
                waiter.resolve(item);
            } else {
                items.push(item);
            }
        },
        next() {
            if (items.length > 0) {
                return Promise.resolve(items.shift());
            }
            return new Promise((resolve, reject) => {
                const waiter = { resolve, timer: undefined };
                waiter.timer = setTimeout(() => {
                    waiters.splice(waiters.indexOf(waiter), 1);
                    reject(new Error(`no ${what} from the session`));
                }, EVENT_TIMEOUT);
                waiters.push(waiter);
            });
        },
    };
}

const pauses = eventQueue("pause");
const exits = eventQueue("exit");
const output = [];
let ourRunStarted = false;

// --- the package and its lines
const projectBuild = build(PROJECT);
const index = new PackageIndex([projectBuild.file], { sourceRoot: PROJECT });

const device = await Device.open(TARGET, { onLog: (stream, text) => output.push(text) });
await device.hello();
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(projectBuild.file));

const session = new DebugSession(device, index, {
    onPaused: pause => pauses.push(pause),
    onStarted: () => {
        ourRunStarted = true;
    },
    onExited: code => {
        if (ourRunStarted) {
            exits.push(code);
        }
    },
    onError: error => check(false, `session error: ${error.message}`),
});
const attached = await session.attach();
check(attached.package !== undefined, "the package installed on the device is found by its build id");

const main = session.package.script("src/main.js");
const shapes = session.package.script("src/shapes.js");
equal(main.check().length, 0, "main.js: source, sidecar and local file are consistent");

// 0-based line of the first line of a script that contains 'text'
function lineOf(script, text) {
    return script.source.split("\n").findIndex(line => line.includes(text));
}

function where(frame) {
    return `${frame.functionName}:${frame.line}`;
}

// --- breakpoints by line, before the run
const blankLine = lineOf(main, "let keepSpinning = true;") + 1;
const slid = await session.setBreakpoint("src/main.js", blankLine);
equal(slid?.location?.line, lineOf(main, "const box = "), "a breakpoint on an empty line slides to the next statement");
await session.removeBreakpoint(slid.id);

const breakpointLine = lineOf(main, "const result = area(box);");
const breakpoint = await session.setBreakpoint("src/main.js", breakpointLine);
equal(breakpoint?.location?.line, breakpointLine, "breakpoint set on its line");
await session.setPauseOnExceptions("none");

await device.request(KIND.RUN);

// --- at the breakpoint: stack by names and lines
let pause = await pauses.next();
equal(pause.reason, "breakpoint", "paused at the breakpoint");
equal(where(pause.frames[0]), `compute:${breakpointLine}`, "frame 0 is compute() at the breakpoint line");
equal(where(pause.frames[1]), `<module>:${lineOf(main, "compute(3, 4)")}`, "frame 1 is the module code at the call");

// --- variables by name
const scopes = await session.scopes(0);
const variables = new Map(scopes.flatMap(scope => scope.variables.map(variable => [variable.name, variable.value])));
equal(variables.get("width")?.value, 3, "argument 'width'");
equal(variables.get("box")?.className, "Object", "'box' is an object");
equal(variables.get("result")?.type, "uninitialized", "'result' in its temporal dead zone");
check(scopes.some(scope => scope.kind === "closure" && scope.variables.some(variable => variable.name === "area")), "the imported 'area' is a closure variable");

const boxProperties = await session.properties(variables.get("box").handle);
equal(boxProperties.properties.map(property => property.name).join(","), "width,height,label", "properties of 'box'");

// --- evaluation in the frame
let evaluated = await session.evaluate("box.width * height");
equal(evaluated.value?.value, 12, "evaluate an expression with the variables of the frame");
evaluated = await session.evaluate("notDefinedAnywhere");
check(evaluated.exception?.description?.startsWith("ReferenceError"), "an evaluation that throws gives the exception");
evaluated = await session.evaluate("1 +");
check(evaluated.exception?.description?.startsWith("SyntaxError"), "a syntax error is reported without reaching the device");
evaluated = await session.evaluate("area", { level: 1 });
equal(evaluated.value?.kind, "function", "evaluate in the calling frame");

// --- change a variable
await session.setVariable(0, "height", 10);
evaluated = await session.evaluate("height");
equal(evaluated.value?.value, 10, "setVariable changed 'height'");

// --- steps by lines
await session.stepInto();
pause = await pauses.next();
equal(`${pause.frames[0].module}:${where(pause.frames[0])}`, `src/shapes.js:area:${lineOf(shapes, "const surface")}`, "step into area()");
await session.stepOut();
pause = await pauses.next();
equal(where(pause.frames[0]), `compute:${lineOf(main, "return result;")}`, "step out of area()");
await session.stepOver();
pause = await pauses.next();
equal(pause.returnValue?.value, 12, "step over to the return of compute(): 3 * 4");

// --- pause of the running loop, then end it through a closure variable
await session.resume();
await new Promise(resolve => setTimeout(resolve, 200));
await session.pause();
pause = await pauses.next();
equal(pause.frames[0].functionName, "spin", "paused in spin()");
await session.setVariable(0, "keepSpinning", false);
await session.resume();

// --- 'debugger;' of the module code (the exception of failSoftly() does not pause)
pause = await pauses.next();
equal(`${pause.reason}:${pause.frames[0].functionName}`, "debugger:<module>", "paused on 'debugger;' of the module");
await session.resume();

// --- step over the end of first(): the next statement of the module
pause = await pauses.next();
equal(pause.frames[0].functionName, "first", "paused on 'debugger;' in first()");
for (let i = 0; i < 5 && pause.frames[0].functionName === "first"; i++) {
    await session.stepOver();
    pause = await pauses.next();
}
equal(where(pause.frames[0]), `<module>:${lineOf(main, "waitAndCount().then(showTicks);")}`, "step over the end of first()");
await session.resume();

// --- async function: step over the 'await', then out
pause = await pauses.next();
equal(pause.frames[0].functionName, "waitAndCount", "paused in waitAndCount()");
await session.stepOver();
pause = await pauses.next();
equal(where(pause.frames[0]), `waitAndCount:${lineOf(main, "await sleep(20);")}`, "step over to the 'await'");
await session.stepOver();
pause = await pauses.next();
equal(where(pause.frames[0]), `waitAndCount:${lineOf(main, "return ticks;")}`, "step over the 'await'");
await session.stepOut();
pause = await pauses.next();
equal(where(pause.frames[0]), `showTicks:${lineOf(main, "console.log(\"ticks\", count);")}`, "step out of the async function");
await session.resume();

// --- end of the program
const code = await exits.next();
equal(code, 0, "exit code");
equal(output.join(""), "area 12\nspun true\nexpected failure\nsum 3\nticks 1\ndone\n", "program output");

await session.detach();
device.close();
fs.rmSync(`${PROJECT}/build`, { recursive: true, force: true });
report("proxy");
