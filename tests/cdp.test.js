/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tests of the CDP server of the debugger (qjsx:cdp) through the desktop
// agent: a WebSocket client plays the part of VS Code's js-debug (startup
// sequence, breakpoint by urlRegex, pause, scopes, evaluation, calls, steps)
// on tests/fixtures/debuggee.
// Run with: bin/Release/qjsx run tests/cdp.test.js tcp:127.0.0.1:<port>
// (an "qjsx agent" must listen there; tests/run-tests.sh starts one).
import * as fs from "fs";
import { connect as netConnect } from "net";
import { resolve } from "stdlib/path";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND } from "qjsx:protocol";
import { PackageIndex } from "qjsx:debuginfo";
import { CdpServer, fileUrl } from "qjsx:cdp";
import { connect } from "qjsx:websocket";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const PROJECT = "tests/fixtures/debuggee";
const EVENT_TIMEOUT = 5000;

// --- the device and the server
const projectBuild = build(PROJECT);
const index = new PackageIndex([projectBuild.file], { sourceRoot: PROJECT });
let server = null;
const device = await Device.open(TARGET, { onLog: (stream, text) => server?.output(stream, text) });
await device.hello();
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(projectBuild.file));

server = new CdpServer({ device, index, port: 0, title: "qjsx test", sourceCache: `${PROJECT}/build/debug-src` });
server.start();

// --- HTTP discovery, as js-debug does it
async function httpGet(path) {
    const connection = await netConnect({ port: server.port() });
    return new Promise(done => {
        let text = "";
        connection.onData(bytes => {
            text += new TextDecoder().decode(bytes);
        });
        connection.onClose(() => done(text));
        connection.write(`GET ${path} HTTP/1.1\r\nHost: localhost\r\n\r\n`);
    });
}

const listAnswer = await httpGet("/json/list");
const list = JSON.parse(listAnswer.slice(listAnswer.indexOf("\r\n\r\n") + 4));
equal(list[0]?.webSocketDebuggerUrl, server.webSocketUrl(), "/json/list gives the WebSocket URL");

// --- the client: commands answered by id, events queued by method
const socket = await connect({ port: server.port(), path: "/qjsx" });
const pending = new Map();
const events = new Map();     // method -> { items, waiters }
let nextId = 1;

function queueOf(method) {
    if (!events.has(method)) {
        events.set(method, { items: [], waiters: [] });
    }
    return events.get(method);
}

socket.onMessage(text => {
    const message = JSON.parse(text);
    if (message.id !== undefined) {
        const answer = pending.get(message.id);
        pending.delete(message.id);
        answer?.(message);
        return;
    }

    const queue = queueOf(message.method);
    const waiter = queue.waiters.shift();
    if (waiter !== undefined) {
        clearTimeout(waiter.timer);
        waiter.resolve(message.params);
    } else {
        queue.items.push(message.params);
    }
});

// a command: { result } or { error }
function send(method, params = {}) {
    const id = nextId++;
    socket.send(JSON.stringify({ id, method, params }));
    return new Promise(done => pending.set(id, done));
}

async function command(method, params) {
    const answer = await send(method, params);
    check(answer.error === undefined, `${method}: ${answer.error?.message ?? "ok"}`);
    return answer.result ?? {};
}

function nextEvent(method) {
    const queue = queueOf(method);
    if (queue.items.length > 0) {
        return Promise.resolve(queue.items.shift());
    }
    return new Promise((done, fail) => {
        const waiter = { resolve: done, timer: undefined };
        waiter.timer = setTimeout(() => {
            queue.waiters.splice(queue.waiters.indexOf(waiter), 1);
            fail(new Error(`no ${method}`));
        }, EVENT_TIMEOUT);
        queue.waiters.push(waiter);
    });
}

// --- startup, in js-debug's order
await command("Runtime.enable");
const context = await nextEvent("Runtime.executionContextCreated");
check(context.context.auxData.isDefault, "a default execution context");
const enabled = await command("Debugger.enable");
check(typeof enabled.debuggerId === "string", "Debugger.enable gives a debuggerId");
await command("Profiler.enable");
await command("Debugger.setAsyncCallStackDepth", { maxDepth: 32 });
const telemetry = await send("Runtime.evaluate", {
    expression: "typeof process === 'undefined' || process.pid === undefined ? 'process not defined' : ({ processId: process.pid })",
    contextId: 1,
    returnByValue: true,
});
// no program runs yet: the device refuses to evaluate, js-debug tolerates the error
check(telemetry.result !== undefined || telemetry.error !== undefined, "js-debug's probe of 'process' gets an answer");
const unknown = await send("Runtime.getHeapUsage");
equal(unknown.error?.code, -32601, "an unknown method: 'not found'");
await command("Debugger.setPauseOnExceptions", { state: "none" });

// --- a breakpoint by urlRegex, as js-debug builds it from the local path
const mainPath = resolve(PROJECT, "src/main.js");
const mainUrl = fileUrl(mainPath);
const mainSource = fs.readFileSync(mainPath, "utf8");
const lineOf = (source, text) => source.split("\n").findIndex(line => line.includes(text));
const escape = text => text.replace(/[.*+?^${}()|[\]\\/]/g, "\\$&");
const urlRegex = `${escape(mainUrl)}($|\\?)|${escape(mainPath)}($|\\?)`;
const breakpointLine = lineOf(mainSource, "const result = area(box);");

const conditional = await send("Debugger.setBreakpointByUrl", { urlRegex, lineNumber: breakpointLine, condition: "width > 1" });
check(conditional.error !== undefined, "a conditional breakpoint is refused (step 4)");
const breakpoint = await command("Debugger.setBreakpointByUrl", { urlRegex, lineNumber: breakpointLine, columnNumber: 0 });
check(breakpoint.breakpointId !== undefined, "breakpoint set by urlRegex");

// --- run: the scripts, the breakpoint resolved, the pause
await device.request(KIND.RUN);
const parsed = await nextEvent("Debugger.scriptParsed");
equal(parsed.url, mainUrl, "src/main.js announced by its file:/// URL");
const resolved = await nextEvent("Debugger.breakpointResolved");
equal(`${resolved.breakpointId}:${resolved.location.lineNumber}`, `${breakpoint.breakpointId}:${breakpointLine}`, "breakpoint resolved in main.js");

let paused = await nextEvent("Debugger.paused");
let top = paused.callFrames[0];
equal(`${top.functionName}:${top.location.lineNumber}`, `compute:${breakpointLine}`, "paused in compute() at the breakpoint");
check(paused.hitBreakpoints?.includes(breakpoint.breakpointId), "hitBreakpoints names the breakpoint");

// --- scopes and objects
const localScope = top.scopeChain.find(scope => scope.type === "local");
const locals = (await command("Runtime.getProperties", { objectId: localScope.object.objectId, ownProperties: true })).result;
const localByName = new Map(locals.map(property => [property.name, property.value]));
equal(localByName.get("width")?.value, 3, "local 'width'");
equal(localByName.get("box")?.className, "Object", "local 'box' is an object");
const boxProperties = (await command("Runtime.getProperties", { objectId: localByName.get("box").objectId, ownProperties: true })).result;
equal(boxProperties.map(property => property.name).join(","), "width,height,label", "properties of 'box'");

// --- evaluation, calls, change of a variable
let evaluated = await command("Debugger.evaluateOnCallFrame", { callFrameId: top.callFrameId, expression: "box.width * height" });
equal(evaluated.result?.value, 12, "evaluateOnCallFrame");
evaluated = await command("Runtime.callFunctionOn", {
    functionDeclaration: "function (extra) { return this.width + extra; }",
    objectId: localByName.get("box").objectId,
    arguments: [{ value: 10 }],
});
equal(evaluated.result?.value, 13, "callFunctionOn with 'this' and an argument");
evaluated = await command("Runtime.callFunctionOn", {
    functionDeclaration: "function () { return { width: this.width, label: this.label }; }",
    objectId: localByName.get("box").objectId,
    returnByValue: true,
});
equal(JSON.stringify(evaluated.result?.value), "{\"width\":3,\"label\":\"box\"}", "callFunctionOn by value");
await command("Debugger.setVariableValue", { scopeNumber: 0, variableName: "height", newValue: { value: 10 }, callFrameId: top.callFrameId });
evaluated = await command("Debugger.evaluateOnCallFrame", { callFrameId: top.callFrameId, expression: "height" });
equal(evaluated.result?.value, 10, "setVariableValue");

// --- sources and possible breakpoints
const source = await command("Debugger.getScriptSource", { scriptId: parsed.scriptId });
equal(source.scriptSource, mainSource, "getScriptSource");
const possible = await command("Debugger.getPossibleBreakpoints", {
    start: { scriptId: parsed.scriptId, lineNumber: breakpointLine, columnNumber: 0 },
    end: { scriptId: parsed.scriptId, lineNumber: breakpointLine + 1, columnNumber: 0 },
});
equal(possible.locations?.[0]?.lineNumber, breakpointLine, "getPossibleBreakpoints on the breakpoint line");

// --- a step, then on
await command("Debugger.stepInto");
await nextEvent("Debugger.resumed");
paused = await nextEvent("Debugger.paused");
equal(paused.callFrames[0].functionName, "area", "stepInto area()");
await command("Debugger.removeBreakpoint", { breakpointId: breakpoint.breakpointId });
await command("Debugger.resume");
await nextEvent("Debugger.resumed");

// --- pause of the running loop, ended through its closure variable
await new Promise(done => setTimeout(done, 200));
await command("Debugger.pause");
paused = await nextEvent("Debugger.paused");
top = paused.callFrames[0];
equal(top.functionName, "spin", "Debugger.pause stops the running loop");
await command("Debugger.setVariableValue", { scopeNumber: 0, variableName: "keepSpinning", newValue: { value: false }, callFrameId: top.callFrameId });
await command("Debugger.resume");

// --- the 'debugger;' statements of the program
for (const name of ["<module>", "first", "waitAndCount"]) {
    await nextEvent("Debugger.resumed");
    paused = await nextEvent("Debugger.paused");
    equal(paused.callFrames[0].functionName, name, `paused on 'debugger;' in ${name}`);
    await command("Debugger.resume");
}

// --- the output of the program in the debug console, then its end
const lines = [];
for (;;) {
    const message = await nextEvent("Runtime.consoleAPICalled");
    lines.push(message.args[0].value);
    if (message.type === "info") {
        break;
    }
}
equal(lines.join("|"), "area 12|spun true|expected failure|sum 3|ticks 1|done|qjsx: program ended (code 0)", "program output as console messages");

socket.close();
await new Promise(done => setTimeout(done, 100));
await server.close();
device.close();
fs.rmSync(`${PROJECT}/build`, { recursive: true, force: true });
report("cdp");
