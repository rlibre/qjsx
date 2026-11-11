/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Debug info of the packages, for the debugger proxy (no device here): finds
// a package by its build id, reads the debug sidecars of its modules and
// converts between the positions of the device (function number + bytecode
// offset) and the positions of the source (line + column).
//
//   const index = new PackageIndex(["build/packages"], { sourceRoot: "." });
//   const pkg = index.find(buildId);              // DebugPackage, or undefined
//   const script = pkg.script("src/main.js");     // ScriptInfo, or undefined
//   script.location(number, pc, { exact: true }) // { line, column, offset }
//   script.breakpointAt(line, column)             // { number, pc, pcs, line, column }
//   script.scopesAt(number, pc, { exact: true })  // visible variables by scope
//
// Lines and columns start at 0 and columns count UTF-16 code units, as in
// CDP. The sidecar gives byte offsets in the UTF-8 source (format in
// host/modules/compiler_module.cpp); each ScriptInfo keeps a table of line
// starts to convert them.
import * as fs from "fs";
import { join } from "stdlib/path";
import { sha256 } from "crypto";
import { readZip } from "qjsx:zip";
import { hex } from "qjsx:util";

// Kinds of the positions of the sidecar (doc/architecture.md 4.4).
const POSITION_EXPRESSION = 0;

// Flags of the variables (JS_DEBUG_VARIABLE_* in quickjs-debug.h).
export const VARIABLE_FLAG = { LEXICAL: 1, CONST: 2, CAPTURED: 4, GLOBAL: 8 };

// Kinds of slots, as SLOT_KIND of qjsx:protocol.
const SLOT_ARGUMENT = 1;
const SLOT_LOCAL = 2;
const SLOT_CLOSURE = 3;

const PACKAGE_EXTENSION = ".qjspkg";
const IDENTIFIER = /^[A-Za-z_$][A-Za-z0-9_$]*$/;

// ### Packages ################################################################

// The packages of some folders and files, indexed by build id. Only the
// manifests are read when indexing; a package is opened when it is asked
// for. A build id not found makes the index look at its folders again (a
// package built since).
//   paths       folders (their *.qjspkg files) and .qjspkg files
//   sourceRoot  folder the paths of the program modules start from (the
//               project): gives ScriptInfo.localPath; none: localPath null
export class PackageIndex {
    constructor(paths, { sourceRoot } = {}) {
        this.paths = paths;
        this.sourceRoot = sourceRoot;
        this.files = new Map();      // build id -> package file
        this.opened = new Map();     // build id -> DebugPackage
        this.scan();
    }

    // Reads the manifest of every package of the paths.
    scan() {
        for (const file of this.packageFiles()) {
            const buildId = readManifest(file)?.buildId;
            if (buildId !== undefined) {
                this.files.set(buildId, file);
            }
        }
    }

    // The .qjspkg files of the paths (a missing folder gives none).
    packageFiles() {
        const files = [];
        for (const path of this.paths) {
            if (!fs.existsSync(path)) {
                continue;
            }

            if (fs.statSync(path).isFile()) {
                files.push(path);
                continue;
            }

            const names = fs.readdirSync(path).filter(name => name.endsWith(PACKAGE_EXTENSION));
            files.push(...names.map(name => join(path, name)));
        }
        return files;
    }

    // The package of 'buildId' (opened once), or undefined.
    find(buildId) {
        const opened = this.opened.get(buildId);
        if (opened !== undefined) {
            return opened;
        }

        // not indexed yet: maybe built since the last scan
        if (!this.files.has(buildId)) {
            this.scan();
        }

        const file = this.files.get(buildId);
        if (file === undefined) {
            return undefined;
        }

        const pkg = new DebugPackage(file, this.sourceRoot);
        this.opened.set(buildId, pkg);
        return pkg;
    }
}

// Manifest of a package file, or undefined if it is not a readable package.
function readManifest(file) {
    try {
        const entries = readZip(fs.readFileSync(file));
        return decodeJson(entries.get("manifest.json"));
    } catch (error) {
        return undefined;
    }
}

function decodeJson(bytes) {
    return JSON.parse(new TextDecoder().decode(bytes));
}

// An opened package: its manifest and its scripts.
export class DebugPackage {
    constructor(file, sourceRoot) {
        const entries = readZip(fs.readFileSync(file));
        const manifest = decodeJson(entries.get("manifest.json"));

        this.file = file;
        this.buildId = manifest.buildId;
        this.main = manifest.main;
        this.manifest = manifest;
        this.scriptsByModule = new Map();
        for (const record of manifest.scripts) {
            const script = new ScriptInfo(record, entries, sourceRoot);
            this.scriptsByModule.set(script.module, script);
        }
    }

    // The script of a module name ("src/main.js", "stdlib/path"), or undefined.
    script(module) {
        return this.scriptsByModule.get(module);
    }

    scripts() {
        return [...this.scriptsByModule.values()];
    }
}

// ### Scripts #################################################################

// One module of a package: its source, its sidecar, and the conversions.
export class ScriptInfo {
    constructor(record, entries, sourceRoot) {
        this.module = record.path;
        this.library = record.library ?? null;
        this.record = record;
        this.source = new TextDecoder().decode(entries.get(record.source));
        this.sidecar = decodeJson(entries.get(record.debug));
        this.functions = this.sidecar.functions;

        // the file on the disk, for the program modules only (a library
        // module comes from its .qjslib: its source is the one of the package)
        const isProgramModule = this.library === null && sourceRoot !== undefined;
        this.localPath = isProgramModule ? join(sourceRoot, record.path) : null;

        this.lines = new LineTable(this.source);
    }

    // Consistency of the script: [] when all is right, else the problems
    // (text): source of the package against the manifest, sidecar against
    // the manifest, local file changed since the build.
    check() {
        const problems = [];
        if (hex(sha256(this.source)) !== this.record.sourceHash) {
            problems.push(`${this.module}: the source in the package does not match the manifest`);
        }

        if (this.sidecar.buildId !== this.record.buildId) {
            problems.push(`${this.module}: the debug sidecar does not match the bytecode`);
        }

        const localExists = this.localPath !== null && fs.existsSync(this.localPath);
        if (localExists && fs.readFileSync(this.localPath, "utf8") !== this.source) {
            problems.push(`${this.localPath}: the file changed since the package was built`);
        }
        return problems;
    }

    // Name shown for a function: its name, "<module>" for the code of the
    // module, "(anonymous)" otherwise.
    functionName(number) {
        const info = this.functions[number];
        if (info === undefined) {
            return "(unknown)";
        }

        const isModuleCode = number === this.functions.length - 1;
        if (isModuleCode) {
            return "<module>";
        }
        return info.name ?? "(anonymous)";
    }

    // Source offset (bytes) of the bytecode offset 'pc' of function
    // 'number': the last position at or before it. 'exact': the instruction
    // at 'pc' (level 0 of a pause), and a stop position (statement or
    // return) at that very pc wins over the expression positions sharing it;
    // else 'pc' follows a call instruction (a calling frame) and the position
    // of that call is wanted.
    offsetAt(number, pc, { exact }) {
        const info = this.functions[number];
        if (info === undefined) {
            return undefined;
        }

        const target = exact ? pc : pc - 1;
        let offset = info.start;
        let stopOffset = undefined;
        for (const position of positionsOf(info)) {
            if (position.pc > target) {
                break;
            }
            offset = position.offset;

            const isStopHere = exact && position.pc === target && position.kind !== POSITION_EXPRESSION;
            if (isStopHere && stopOffset === undefined) {
                stopOffset = position.offset;
            }
        }
        return stopOffset ?? offset;
    }

    // { line, column, offset } of a device position, or undefined if the
    // function is unknown.
    location(number, pc, { exact = true } = {}) {
        const offset = this.offsetAt(number, pc, { exact });
        if (offset === undefined) {
            return undefined;
        }

        const { line, column } = this.lines.position(offset);
        return { line, column, offset };
    }

    // Where a breakpoint asked at (line, column) goes: the first stop
    // position (statement or return) at or after it, in the innermost
    // function that has one there. A line without statement slides to the
    // next one, as in V8. null if there is none until the end.
    //   pcs: every offset of that function at that source position
    breakpointAt(line, column = 0) {
        const wanted = this.lines.offset(line, column);

        // the first stop position from there, innermost function on a tie
        let best = null;
        for (const info of this.functions) {
            for (const position of positionsOf(info)) {
                if (position.kind === POSITION_EXPRESSION || position.offset < wanted) {
                    continue;
                }

                const isBetter = best === null || position.offset < best.offset
                    || (position.offset === best.offset && isInside(info, best.info));
                if (isBetter) {
                    best = { info, offset: position.offset };
                }
            }
        }

        if (best === null) {
            return null;
        }

        // every stop of that function at that position
        const pcs = positionsOf(best.info)
            .filter(position => position.kind !== POSITION_EXPRESSION && position.offset === best.offset)
            .map(position => position.pc);
        const found = this.lines.position(best.offset);
        return { number: best.info.number, pc: pcs[0], pcs, line: found.line, column: found.column };
    }

    // Stops of SET_STOPS STATEMENTS: every stop position of every function.
    statementStops() {
        return this.functions.map(info => ({
            number: info.number,
            offsets: positionsOf(info).filter(position => position.kind !== POSITION_EXPRESSION).map(position => position.pc),
        }));
    }

    // The variables visible at a device position, by scope, innermost first:
    //   [{ kind: "block" | "local" | "closure", variables: [{ name, slotKind, index, flags }] }]
    // "block": the scopes inside the function that contain the position;
    // "local": the arguments and the variables of the function scope;
    // "closure": the variables the function takes from enclosing functions
    // (module variables included). Internal variables ("<ret>"...) and 'this'
    // are left out (see thisSlot). Empty scopes are left out.
    scopesAt(number, pc, { exact = true } = {}) {
        const info = this.functions[number];
        if (info === undefined) {
            return [];
        }

        // the scopes of the function that contain the position, innermost
        // first (a scope contains its children)
        const offset = this.offsetAt(number, pc, { exact });
        const containing = info.scopes
            .map((scope, index) => ({ scope, index }))
            .filter(({ scope }) => scope.start <= offset && offset < scope.end)
            .sort((a, b) => (b.scope.start - a.scope.start) || (a.scope.end - b.scope.end));

        // a block scope per containing inner scope, then the function scope
        // with the arguments
        const result = [];
        for (const { index } of containing) {
            if (index === 0) {
                continue;
            }
            const variables = slotsOf(info.locals, SLOT_LOCAL).filter(variable => variable.scope === index);
            result.push({ kind: "block", variables });
        }

        const functionScope = slotsOf(info.locals, SLOT_LOCAL).filter(variable => variable.scope === 0);
        result.push({ kind: "local", variables: [...slotsOf(info.arguments, SLOT_ARGUMENT), ...functionScope] });
        result.push({ kind: "closure", variables: slotsOf(info.closures, SLOT_CLOSURE) });

        // without the scope numbers, and without empty scopes
        return result
            .map(entry => ({ kind: entry.kind, variables: entry.variables.map(withoutScope) }))
            .filter(entry => entry.variables.length > 0);
    }

    // The slot of 'this' in a function ({ slotKind, index }), or null: QuickJS
    // only makes one when the function uses 'this' (a local variable, or a
    // closure variable in an arrow function).
    thisSlot(number) {
        const info = this.functions[number];
        if (info === undefined) {
            return null;
        }

        const localIndex = info.locals.findIndex(variable => variable.name === "this");
        if (localIndex >= 0) {
            return { slotKind: SLOT_LOCAL, index: localIndex };
        }

        const closureIndex = info.closures.findIndex(variable => variable.name === "this");
        if (closureIndex >= 0) {
            return { slotKind: SLOT_CLOSURE, index: closureIndex };
        }
        return null;
    }
}

// The positions of a sidecar function as records { pc, offset, kind }, sorted
// by pc (the sidecar has flat triples).
function positionsOf(info) {
    if (info.positionRecords === undefined) {
        const records = [];
        for (let i = 0; i < info.positions.length; i += 3) {
            records.push({ pc: info.positions[i], offset: info.positions[i + 1], kind: info.positions[i + 2] });
        }
        info.positionRecords = records;
    }
    return info.positionRecords;
}

// True if function 'inner' is inside function 'outer' (source ranges).
function isInside(inner, outer) {
    const strictlySmaller = inner.end - inner.start < outer.end - outer.start;
    return inner.start >= outer.start && inner.end <= outer.end && strictlySmaller;
}

// The variables of one slot kind that a user can see, with their slot:
// { name, slotKind, index, flags, scope }. Internal names (not
// identifiers: "<ret>", "new.target"...) and 'this' are left out.
function slotsOf(variables, slotKind) {
    const visible = [];
    variables.forEach((variable, index) => {
        const isUserName = typeof variable.name === "string" && IDENTIFIER.test(variable.name) && variable.name !== "this";
        if (isUserName) {
            visible.push({ name: variable.name, slotKind, index, flags: variable.flags, scope: variable.scope });
        }
    });
    return visible;
}

function withoutScope({ name, slotKind, index, flags }) {
    return { name, slotKind, index, flags };
}

// ### Lines ###################################################################

// The character at 'index' of 'text': its UTF-8 length ('bytes') and its
// number of UTF-16 code units ('units': 2 for a pair of surrogates, which is
// 4 bytes in UTF-8).
function characterAt(text, index) {
    const unit = text.charCodeAt(index);
    if (unit < 0x80) {
        return { bytes: 1, units: 1 };
    }
    if (unit < 0x800) {
        return { bytes: 2, units: 1 };
    }

    const isHighSurrogate = unit >= 0xd800 && unit <= 0xdbff;
    const next = text.charCodeAt(index + 1);
    const followedByLow = next >= 0xdc00 && next <= 0xdfff;
    if (isHighSurrogate && followedByLow) {
        return { bytes: 4, units: 2 };
    }
    return { bytes: 3, units: 1 };
}

// Line starts of a source, in bytes (UTF-8) and in UTF-16 code units; lines
// end at '\n'.
export class LineTable {
    constructor(text) {
        this.text = text;
        this.byteStarts = [0];
        this.unitStarts = [0];

        let bytes = 0;
        let unit = 0;
        while (unit < text.length) {
            const character = characterAt(text, unit);
            const isNewLine = text.charCodeAt(unit) === 0x0a;
            bytes += character.bytes;
            unit += character.units;
            if (isNewLine) {
                this.byteStarts.push(bytes);
                this.unitStarts.push(unit);
            }
        }
        this.byteLength = bytes;
    }

    // { line, column } of a byte offset (a column in UTF-16 code units).
    position(offset) {
        // the last line starting at or before the offset (binary search)
        let low = 0;
        let high = this.byteStarts.length - 1;
        while (low < high) {
            const middle = (low + high + 1) >> 1;
            if (this.byteStarts[middle] <= offset) {
                low = middle;
            } else {
                high = middle - 1;
            }
        }

        // count the code units of the line until the offset
        let bytes = this.byteStarts[low];
        let unit = this.unitStarts[low];
        while (bytes < offset && unit < this.text.length) {
            const character = characterAt(this.text, unit);
            bytes += character.bytes;
            unit += character.units;
        }
        return { line: low, column: unit - this.unitStarts[low] };
    }

    // Byte offset of (line, column); past the end of a line or of the text,
    // the nearest end.
    offset(line, column) {
        if (line >= this.byteStarts.length) {
            return this.byteLength;
        }

        // add the UTF-8 length of the first 'column' code units of the line
        const lineStart = this.unitStarts[line];
        const lineEnd = line + 1 < this.unitStarts.length ? this.unitStarts[line + 1] : this.text.length;
        let bytes = this.byteStarts[line];
        let unit = lineStart;
        while (unit < lineEnd && unit - lineStart < column) {
            const character = characterAt(this.text, unit);
            bytes += character.bytes;
            unit += character.units;
        }
        return bytes;
    }
}
