/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tests of a device with small messages (a board without PSRAM): an agent
// started with "qjsx agent --max-message 4096" must listen at the target.
// A generated program has more statement stops than one message holds, a
// console line longer than a message and an object whose properties do not
// all fit; a request too large is refused by the tool.
// Run with: bin/Debug/qjsx run -A tests/maxmessage.test.js tcp:127.0.0.1:<port>
import * as fs from "sys/fs";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND, KEY } from "qjsx:protocol";
import { PackageIndex } from "qjsx:debuginfo";
import { DebugSession } from "qjsx:debugsession";
import { check, equal, rejects, report } from "./assert.js";

const TARGET = process.argv[2];
const MAX_MESSAGE = 4096;                 // what the agent was started with
const PROJECT = "build/maxmessage-project";
const FUNCTION_COUNT = 40;                // 40 functions of 50 statements: about 8 KB of stops
const STATEMENTS = 50;
const EVENT_TIMEOUT = 5000;

// --- the program: many statements, a long line, an object of long strings
function programSource() {
    const lines = [];
    for (let f = 0; f < FUNCTION_COUNT; f++) {
        lines.push(`function f${f}() {`);
        lines.push("    let a = 0;");
        for (let s = 0; s < STATEMENTS; s++) {
            lines.push("    a++;");
        }
        lines.push("    return a;");
        lines.push("}");
    }
    // every function stays in the package: 'qjsx build' leaves out what
    // nothing uses
    const names = Array.from({ length: FUNCTION_COUNT }, (unused, f) => `f${f}`);
    lines.push(`globalThis.allFunctions = [${names.join(", ")}];`);
    lines.push(`const longLine = "é€😀-".repeat(3000);`);
    lines.push("const big = {};");
    lines.push(`for (const name of "abcdefgh") { big[name] = name.repeat(3000); }`);
    lines.push("big.count = 1;");
    lines.push("console.log(longLine);");
    lines.push("debugger;");
    lines.push(`console.log("last", f${FUNCTION_COUNT - 1}(), big.count);`);
    return lines.join("\n") + "\n";
}

fs.rmSync(PROJECT, { recursive: true, force: true });
fs.mkdirSync(`${PROJECT}/src`, { recursive: true });
fs.writeFileSync(`${PROJECT}/qjsx.json`, JSON.stringify({ name: "bigmodule", main: "src/main.js", keep: 1 }));
const source = programSource();
fs.writeFileSync(`${PROJECT}/src/main.js`, source);

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

// --- the device says its largest message
const projectBuild = build(PROJECT);
const index = new PackageIndex([projectBuild.file], { sourceRoot: PROJECT });
const device = await Device.open(TARGET, { onLog: (stream, text) => output.push(text) });
await device.hello();
equal(device.maxMessage, MAX_MESSAGE, "HELLO gives the largest message of the device");

// --- a request larger than that is refused by the tool
await rejects(device.request(KIND.LOAD_FUNCTION, { [KEY.DATA]: new Uint8Array(MAX_MESSAGE) }), /more than the device takes/,
    "a request larger than the device's messages is refused");

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
await session.attach();
const main = session.package.script("src/main.js");
check(main.statementStops().reduce((total, entry) => total + 6 + entry.offsets.length * 4, 0) > MAX_MESSAGE,
    "the statement stops of the module do not fit in one message");

// 0-based line of the last line of the script that contains 'text'
function lastLineOf(text) {
    const lines = main.source.split("\n");
    for (let i = lines.length - 1; i >= 0; i--) {
        if (lines[i].includes(text)) {
            return i;
        }
    }
    return -1;
}

// --- a breakpoint in the last function: its stops go in the last message
const lastFunction = `f${FUNCTION_COUNT - 1}`;
const returnLine = lastLineOf("return a;");
const breakpoint = await session.setBreakpoint("src/main.js", returnLine);
equal(breakpoint?.location?.line, returnLine, "breakpoint in the last function");

await device.request(KIND.RUN);

// --- 'debugger;': the long line arrived whole before it, in pieces
let pause = await pauses.next();
equal(pause.reason, "debugger", "paused on 'debugger;'");
const longLine = "é€😀-".repeat(3000);
equal(output.join(""), `${longLine}\n`, "a console line longer than a message arrives whole (UTF-8 kept)");

// --- the properties of 'big': cut strings, as many as fit
const evaluated = await session.evaluate("big");
const properties = await session.properties(evaluated.value.handle);
const names = properties.properties.map(property => property.name);
check(names.length > 0 && names.length < 9, `some of the properties of 'big' fit in a message (${names.join(",")})`);
check(properties.properties.every(property => (property.value?.value?.length ?? 0) <= MAX_MESSAGE / 4),
    "strings are cut to a quarter of a message");

// --- steps inside the last function (its statement stops arrived)
await session.resume();
pause = await pauses.next();
equal(`${pause.reason}:${pause.frames[0].functionName}:${pause.frames[0].line}`, `breakpoint:${lastFunction}:${returnLine}`,
    "paused at the breakpoint of the last function");
await session.stepOver();
pause = await pauses.next();
equal(pause.returnValue?.value, STATEMENTS, "step over to the return of the last function");
await session.resume();

// --- end of the program
const code = await exits.next();
equal(code, 0, "exit code");
check(output.join("").endsWith(`last ${STATEMENTS} 1\n`), "program output");

await session.detach();
device.close();
fs.rmSync(PROJECT, { recursive: true, force: true });
report("maxmessage");
