/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tests of qjsx:debuginfo without device: lines and UTF-16 columns of a
// source with accents and an emoji, where breakpoints go, positions of the
// device back to lines, visible variables, consistency checks.
// Run with: bin/Release/qjsx run tests/debuginfo.test.js
import { ScriptInfo, LineTable } from "qjsx:debuginfo";
import { compileModule } from "qjsx:compile";
import { check, equal, report } from "./assert.js";

const encoder = new TextEncoder();

const lines = [
    "// é et 😀 avant le code",           // 0
    "function greet(name) {",             // 1
    "    const text = \"😀 \" + name;",  // 2
    "    if (name) {",                    // 3
    "        let inner = text.length;",   // 4
    "        return inner;",              // 5
    "    }",                              // 6
    "    return 0;",                      // 7
    "}",                                  // 8
    "",                                   // 9
    "export const value = greet(\"é\");", // 10
];
const source = lines.join("\n");

// --- lines: byte offsets <-> (line, UTF-16 column)
const table = new LineTable(source);
const nameColumn = lines[2].lastIndexOf("name");
const textBefore = lines.slice(0, 2).join("\n") + "\n" + lines[2].slice(0, nameColumn);
const nameOffset = encoder.encode(textBefore).length;
equal(table.offset(2, nameColumn), nameOffset, "offset of a column after an emoji");
const back = table.position(nameOffset);
equal(`${back.line}:${back.column}`, `2:${nameColumn}`, "line and UTF-16 column of that offset");
const afterEmoji = table.position(encoder.encode(lines[0].slice(0, 10)).length);
equal(afterEmoji.column, 10, "column after an emoji counts its two UTF-16 units");
equal(table.offset(99, 0), encoder.encode(source).length, "a line past the end gives the end");

// --- a script as in a package (the record of the manifest and its entries)
const compiled = compileModule("src/greet.js", source);
const record = {
    path: "src/greet.js",
    buildId: compiled.buildId,
    sourceHash: compiled.sourceHash,
    debug: "debug/src/greet.js.json",
    source: "src/src/greet.js",
};
const entries = new Map([
    [record.source, encoder.encode(source)],
    [record.debug, encoder.encode(JSON.stringify(compiled.sidecar))],
]);
const script = new ScriptInfo(record, entries, undefined);
equal(script.check().length, 0, "consistent script: no problem");
equal(script.localPath, null, "no source root: no local path");

const broken = new ScriptInfo({ ...record, sourceHash: "0" }, entries, undefined);
equal(broken.check().length, 1, "a source that does not match the manifest is reported");

// --- names of the functions
const greetNumber = script.functions.findIndex(info => info.name === "greet");
check(greetNumber >= 0, "greet() is in the sidecar");
equal(script.functionName(greetNumber), "greet", "name of greet()");
equal(script.functionName(script.functions.length - 1), "<module>", "name of the module code");

// --- breakpoints
const atReturn = script.breakpointAt(5, 0);
equal(`${atReturn?.number}:${atReturn?.line}:${atReturn?.column}`, `${greetNumber}:5:8`, "breakpoint on 'return inner;'");
check(atReturn.pcs.length >= 1 && atReturn.pcs.includes(atReturn.pc), "breakpoint offsets of that statement");
const slid = script.breakpointAt(9, 0);
equal(slid?.line, 10, "a breakpoint on an empty line slides to the next statement");

// --- device positions back to lines
const location = script.location(atReturn.number, atReturn.pc);
equal(`${location.line}:${location.column}`, "5:8", "location of the breakpoint offset");

// --- visible variables
const scopes = script.scopesAt(atReturn.number, atReturn.pc);
const describe = scopes.map(scope => `${scope.kind}(${scope.variables.map(variable => variable.name).join(",")})`).join(" ");
check(scopes[0].kind === "block" && scopes[0].variables.some(variable => variable.name === "inner"), `innermost scope holds 'inner': ${describe}`);
const allNames = scopes.flatMap(scope => scope.variables.map(variable => variable.name));
check(allNames.includes("name") && allNames.includes("text"), `'name' and 'text' are visible: ${describe}`);
check(!allNames.some(name => name.startsWith("<")), "internal variables are hidden");
const atStart = script.scopesAt(greetNumber, script.breakpointAt(2, 0).pc);
check(!atStart.flatMap(scope => scope.variables).some(variable => variable.name === "inner"), "'inner' is not visible before its block");

// --- statement stops
const stops = script.statementStops();
equal(stops.length, script.functions.length, "statement stops for every function");
check(stops[greetNumber].offsets.includes(atReturn.pc), "the breakpoint offset is a statement stop");

report("debuginfo");
