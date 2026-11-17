/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tests of what 'qjsx build' leaves out of a package (host/js/prune.js, over
// the outline of the modules, engine change W) and of the check of the
// imported names. tests/run-tests.sh also runs the package of the fixture
// against its sources.
// Run with: bin/Debug/qjsx run -A tests/prune.test.js
import * as fs from "sys/fs";
import { compile } from "sys/compiler";
import { build } from "qjsx:build";
import { readZip } from "qjsx:zip";
import { check, equal, throws, report } from "./assert.js";

const FIXTURE = "tests/fixtures/prune";
const decoder = new TextDecoder();

// The outline of one module.
function outlineOf(source) {
    return compile(source, "probe/module", { module: true, strip: "all", outline: true }).outline;
}

// The statement of an outline whose text starts with 'start'.
function statementStarting(source, outline, start) {
    const bytes = new TextEncoder().encode(source);
    return outline.statements.find(statement => decoder.decode(bytes.subarray(statement.start, statement.end)).startsWith(start));
}

// --- the outline: removable declarations, names declared and used
{
    const source = [
        "import { a } from './a.js';",
        "export function f() { return g() + a; }",
        "function g() { return 1; }",
        "const K = 5, S = 'text', O = { x: [1, 2] };",
        "class C extends Base { m() {} [Symbol.iterator]() {} }",
        "class Base {}",
        "class WithStatic { static field = compute(); }",
        "const called = compute();",
        "let assigned; const sneaky = (assigned = 1);",
        "export default function () {}",
        "export { g as gee, K, };",
        "function compute() { return 0; }",
        "console.log(f());",
    ].join("\n");
    const outline = outlineOf(source);
    const removable = start => statementStarting(source, outline, start)?.removable;

    equal(removable("import"), false, "an import is never removable");
    equal(removable("export function f"), true, "a function declaration is removable");
    equal(removable("const K"), true, "literals, objects and arrays of them are removable");
    equal(removable("class C"), true, "a class with a heritage and a computed key that reads is removable");
    equal(removable("class WithStatic"), false, "a static field runs code: kept");
    equal(removable("const called"), false, "a call: kept");
    equal(removable("const sneaky"), false, "an assignment to another name: kept");
    equal(removable("export default function"), true, "export default of a function is removable");
    equal(removable("console.log"), false, "an expression statement is kept");

    const f = statementStarting(source, outline, "export function f");
    equal(f.declared.join(), "f", "names declared");
    check(f.references.includes("g") && f.references.includes("a"), "names used by a function body");
    equal(statementStarting(source, outline, "export default").declared.join(), "*default*", "export default declares *default*");

    equal(outline.imports.map(entry => `${entry.imported}>${entry.local}@${entry.position}`).join(), "a>a@9",
        "imports with the position of the imported name");
    const clause = outline.exports.find(entry => entry.exported === "gee");
    equal(source.slice(clause.start, clause.end), "g as gee,", "a name of an export clause, with its comma");
    equal(outline.hasEval, false, "no eval");
    equal(outlineOf("export function e(t) { return eval(t); }").hasEval, true, "a direct eval is seen");
}

// --- the build of the fixture: what is left out
const built = build(FIXTURE);
equal(built.notes.join("\n"), [
    "src/everything.js: left out, not used: local",
    "src/lib.js: left out, not used: unusedHelper, UnusedShape, UNUSED_CONSTANT, default",
    "src/main.js: left out, not used: unusedInMain",
    "src/source.js: left out, not used: forgotten",
    "src/star.js: left out, not used: notStarred",
    "stdlib/tasks: left out, not used: STOP_WARNING_DELAY, checkDuration, Task, Ticker, Mailbox, task",
].join("\n"), "what the build leaves out (not: 'import * as' exports, a module with eval and what it imports, side effects)");

// the package keeps the original sources: the debugger shows them
const entries = readZip(fs.readFileSync(built.file));
const packagedLib = decoder.decode(entries.get("src/src/lib.js"));
equal(packagedLib, fs.readFileSync(`${FIXTURE}/src/lib.js`, "utf8"), "the package holds the original source");
const manifest = JSON.parse(decoder.decode(entries.get("manifest.json")));
const libScript = manifest.scripts.find(script => script.path === "src/lib.js");
const libSidecar = JSON.parse(decoder.decode(entries.get(libScript.debug)));
equal(libSidecar.buildId, libScript.buildId, "the sidecar matches the bytecode compiled again");
check(libSidecar.functions.every(info => info.name !== "unusedHelper"), "no function left out in the sidecar");

// a second build gives the same package
equal(build(FIXTURE).buildId, built.buildId, "the build stays reproducible");

// --- an imported name that the module does not export
throws(() => build("tests/fixtures/prune-missing"), /^Error: src\/main\.js:10:19: 'absent' is not exported by src\/lib\.js$/,
    "an import of a missing name is refused at its position");

report("prune");
