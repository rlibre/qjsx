/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Splits quickjs.c (QuickJS 2026-06-04) into several translation units.
// Usage: node split.js <quickjs.c> <output dir>
//
// Steps:
//  1. cut the file into top-level units (functions, data, types, macros, prototypes),
//     each one remembering the #if/#elif/#else context it lives in;
//  2. apply the agreed source patches (JSON parser state, module exports);
//  3. assign every unit to an output file (line ranges + overrides by name);
//  4. compute what is shared between files: shared functions/data lose 'static'
//     and get a declaration in a header, shared types/macros/inline functions
//     move to a header;
//  5. write the files.
'use strict';
const fs = require('fs');
const path = require('path');
const config = require('./split-config.js');

const [, , inputPath, outputDir] = process.argv;

// ---------------------------------------------------------------------------
// Text helpers

// Replaces comments and string/char literals with spaces, keeping newlines.
function stripCommentsAndStrings(text) {
    let out = '';
    let i = 0;
    while (i < text.length) {
        const c = text[i];
        const next = text[i + 1];
        if (c === '/' && next === '*') {
            const end = text.indexOf('*/', i + 2);
            const stop = end < 0 ? text.length : end + 2;
            out += text.slice(i, stop).replace(/[^\n]/g, ' ');
            i = stop;
        } else if (c === '/' && next === '/') {
            const end = text.indexOf('\n', i);
            const stop = end < 0 ? text.length : end;
            out += ' '.repeat(stop - i);
            i = stop;
        } else if (c === '"' || c === "'") {
            let j = i + 1;
            while (j < text.length && text[j] !== c && text[j] !== '\n') {
                j += text[j] === '\\' ? 2 : 1;
            }
            out += ' '.repeat(j + 1 - i);
            i = j + 1;
        } else {
            out += c;
            i++;
        }
    }
    return out;
}

function removeAttributes(text) {
    let result = text;
    let start = result.indexOf('__attribute__');
    while (start >= 0) {
        let i = result.indexOf('(', start);
        let depth = 0;
        do {
            if (result[i] === '(') depth++;
            if (result[i] === ')') depth--;
            i++;
        } while (depth > 0 && i < result.length);
        result = result.slice(0, start) + result.slice(i);
        start = result.indexOf('__attribute__');
    }
    return result;
}

const NOT_A_FUNCTION_NAME = new Set([
    'if', 'while', 'for', 'switch', 'return', 'sizeof', 'defined',
]);

function functionName(header) {
    const cleaned = removeAttributes(header);
    const candidates = [...cleaned.matchAll(/([A-Za-z_]\w*)\s*\(/g)];
    const named = candidates.find(m => !NOT_A_FUNCTION_NAME.has(m[1]));
    return named ? named[1] : null;
}

// True if 'text' contains '=' outside of parentheses and brackets.
function hasTopLevelEquals(text) {
    let depth = 0;
    for (let i = 0; i < text.length; i++) {
        const c = text[i];
        if (c === '(' || c === '[') depth++;
        if (c === ')' || c === ']') depth--;
        const isAssignment = c === '=' && text[i + 1] !== '=' && !'!<>='.includes(text[i - 1]);
        if (depth === 0 && isAssignment) {
            return true;
        }
    }
    return false;
}

// Identifier declared by a data definition/declaration header ("static const T name[N] =").
function dataName(header) {
    const beforeInit = removeAttributes(header).split('=')[0].replace(/;\s*$/, '');
    const match = beforeInit.match(/([A-Za-z_]\w*)\s*(\[[^\]]*\]\s*)*$/);
    return match ? match[1] : null;
}

// Constants declared by an enum definition: first identifier of each item.
function enumeratorsOf(code) {
    const body = code.slice(code.indexOf('{') + 1, code.lastIndexOf('}'));
    const withoutDirectives = body.split('\n').filter(l => !l.trim().startsWith('#')).join('\n');
    return withoutDirectives.split(',')
        .map(item => (item.trim().match(/^([A-Za-z_]\w*)/) || [])[1])
        .filter(Boolean);
}

// Identifiers used by a piece of code, ignoring struct member accesses.
function identifiersOf(code) {
    const found = new Set();
    for (const m of code.matchAll(/(\.|->)?\s*\b([A-Za-z_]\w*)/g)) {
        const isMemberAccess = m[1] !== undefined;
        if (!isMemberAccess) {
            found.add(m[2]);
        }
    }
    return found;
}

// ---------------------------------------------------------------------------
// 1. Cutting into units

const CONDITIONAL_OPEN = new Set(['if', 'ifdef', 'ifndef']);
const CONDITIONAL_BRANCH = new Set(['elif', 'else', 'elifdef', 'elifndef']);

// A unit: { kind, names, text, code, header, cond, sortKey, lineIndices, ... }
// kind: func | proto | data | type | macro | undef | pp
function parseUnits(text, firstLineNumber) {
    const originalLines = text.split('\n');
    const codeLines = stripCommentsAndStrings(text).split('\n');
    const units = [];
    const warnings = [];
    const condStack = [];
    let pending = [];

    const snapshotCond = () => condStack.map(frame => [...frame]);
    // line splicing happens before comment removal: read the original text
    const directiveEnd = (start) => {
        let end = start;
        while (originalLines[end].trimEnd().endsWith('\\') && end + 1 < originalLines.length) {
            end++;
        }
        return end;
    };
    const range = (a, b) => Array.from({ length: b - a + 1 }, (_, k) => a + k);

    let i = 0;
    while (i < codeLines.length) {
        const trimmed = codeLines[i].trim();
        if (trimmed === '') {
            pending.push(i);
            i++;
            continue;
        }
        if (trimmed.startsWith('#')) {
            const end = directiveEnd(i);
            const directiveText = range(i, end).map(k => originalLines[k]).join('\n');
            const keyword = trimmed.match(/^#\s*(\w+)/)[1];
            if (CONDITIONAL_OPEN.has(keyword)) {
                condStack.push([directiveText]);
            } else if (CONDITIONAL_BRANCH.has(keyword)) {
                condStack[condStack.length - 1].push(directiveText);
            } else if (keyword === 'endif') {
                condStack.pop();
            } else {
                units.push({
                    kind: keyword === 'define' ? 'macro' : keyword === 'undef' ? 'undef' : 'pp',
                    lineIndices: pending.concat(range(i, end)),
                    codeStart: i,
                    cond: snapshotCond(),
                });
                pending = [];
            }
            i = end + 1;
            continue;
        }

        // a C declaration or definition
        let depth = 0;
        let sawBrace = false;
        let header = '';
        let isFunction = false;
        let end = -1;
        let j = i;
        for (; j < codeLines.length && end < 0; j++) {
            const line = codeLines[j];
            if (line.trim().startsWith('#')) {
                const keyword = line.trim().match(/^#\s*(\w+)/)[1];
                const isConditional = CONDITIONAL_OPEN.has(keyword) || CONDITIONAL_BRANCH.has(keyword) || keyword === 'endif';
                if (isConditional && depth === 0) {
                    warnings.push(`conditional directive inside a declaration header at line ${j + firstLineNumber}`);
                }
                j = directiveEnd(j);
                continue;
            }
            for (let k = 0; k < line.length; k++) {
                const c = line[k];
                if (c === '{') {
                    if (depth === 0 && !sawBrace) {
                        sawBrace = true;
                        header += line.slice(0, k);
                        isFunction = !hasTopLevelEquals(header) && header.includes('(') && !/^\s*typedef\b/.test(header);
                    }
                    depth++;
                } else if (c === '}') {
                    depth--;
                    if (depth === 0 && isFunction) {
                        end = j;
                        break;
                    }
                } else if (c === ';' && depth === 0) {
                    end = j;
                    break;
                }
            }
            if (!sawBrace) {
                header += line + '\n';
            }
        }
        if (end < 0) {
            throw new Error(`unterminated declaration starting at line ${i + firstLineNumber}`);
        }
        units.push({
            kind: sawBrace ? (isFunction ? 'func' : 'braced') : 'statement',
            header,
            lineIndices: pending.concat(range(i, end)),
            codeStart: i,
            cond: snapshotCond(),
        });
        pending = [];
        i = end + 1;
    }

    for (const unit of units) {
        unit.text = unit.lineIndices.map(k => originalLines[k]).join('\n');
        unit.startLine = unit.codeStart + firstLineNumber;
        unit.sortKey = unit.startLine;
        analyzeUnit(unit);
    }
    return { units, warnings, trailing: pending.map(k => originalLines[k]).join('\n') };
}

// Computes kind, names, static/inline flags and references from unit.text.
function analyzeUnit(unit) {
    unit.code = stripCommentsAndStrings(unit.text);
    const code = unit.code;
    unit.names = [];
    unit.isStatic = false;
    unit.isInline = false;

    if (unit.kind === 'macro' || unit.kind === 'undef') {
        const m = code.match(/#\s*(?:define|undef)\s+([A-Za-z_]\w*)(\(([^)]*)\))?/);
        unit.names = [m[1]];
        const params = new Set((m[3] || '').split(',').map(s => s.trim()));
        const body = code.slice(code.indexOf(m[0]) + m[0].length);
        unit.refs = new Set([...identifiersOf(body)].filter(id => !params.has(id)));
    } else if (unit.kind === 'pp') {
        unit.refs = new Set();
    } else {
        const firstBrace = code.indexOf('{');
        const header = unit.kind === 'statement' ? code : code.slice(0, firstBrace);
        unit.header = header;
        const headerNoAttr = removeAttributes(header).trim();
        unit.isStatic = /^static\b/.test(headerNoAttr) || /^\S*\s*static\b/.test(headerNoAttr);
        unit.isInline = /\b(inline|force_inline|__inline__|__always_inline__)\b/.test(headerNoAttr);
        if (unit.kind === 'func') {
            unit.names = [functionName(header)];
        } else if (unit.kind === 'braced') {
            if (hasTopLevelEquals(header)) {
                unit.kind = 'data';
                unit.names = [dataName(header)];
            } else {
                unit.kind = 'type';
                const tag = header.match(/\b(struct|union|enum)\s+([A-Za-z_]\w*)/);
                const typedefName = code.match(/}\s*([A-Za-z_]\w*)\s*;\s*$/);
                unit.names = [tag && tag[2], typedefName && typedefName[1]].filter(Boolean);
                if (/\benum\b/.test(header)) {
                    unit.names.push(...enumeratorsOf(code));
                }
            }
        } else { // statement without braces
            const pointerName = header.match(/\(\s*\*\s*([A-Za-z_]\w*)\s*\)/);
            if (/^\s*typedef\b/.test(header)) {
                unit.kind = 'type';
                const name = pointerName ? pointerName[1]
                    : header.includes('(') ? functionName(header.replace(/^\s*typedef\b/, ''))
                    : (header.match(/([A-Za-z_]\w*)\s*;\s*$/) || [])[1];
                unit.names = [name];
            } else if (header.includes('(') && !hasTopLevelEquals(header) && !pointerName) {
                unit.kind = 'proto';
                unit.names = [functionName(header)];
            } else {
                unit.kind = hasTopLevelEquals(header) ? 'data' : 'data-decl';
                unit.names = [pointerName ? pointerName[1] : dataName(header)];
            }
        }
        const own = new Set(unit.names);
        unit.refs = new Set([...identifiersOf(code)].filter(id => !own.has(id)));
    }
    // macros tested by the #if context are references too
    for (const frame of unit.cond) {
        for (const directive of frame) {
            const condition = stripCommentsAndStrings(directive).replace(/^\s*#\s*\w+/, '');
            for (const id of identifiersOf(condition)) {
                if (id !== 'defined') unit.refs.add(id);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Prototype / declaration text generation

function prototypeOf(unit, keepStatic) {
    const header = unit.header.replace(/\s+$/, '');
    const lines = header.split('\n').map(l => l.replace(/\s+$/, '')).filter(l => l.trim() !== '');
    let text = lines.join('\n') + ';';
    if (!keepStatic) {
        text = text.replace(/\bstatic\s+/, '');
    }
    return text;
}

// Number of items of an array initializer, or null if it cannot be counted
// (preprocessor directives inside the initializer).
function initializerItemCount(unit) {
    const code = unit.code;
    const start = code.indexOf('{', code.indexOf('='));
    let depth = 0;
    let items = 0;
    let currentItemHasContent = false;
    for (let i = start; i < code.length; i++) {
        const c = code[i];
        if (c === '#' && depth === 1) return null;
        if (c === '{' || c === '(' || c === '[') {
            depth++;
            if (depth === 1) continue;
        }
        if (c === '}' || c === ')' || c === ']') {
            depth--;
            if (depth === 0) break;
        }
        if (depth === 1 && c === ',') {
            items++;
            currentItemHasContent = false;
        } else if (!/\s/.test(c)) {
            currentItemHasContent = true;
        }
    }
    return currentItemHasContent ? items + 1 : items;
}

function externDeclarationOf(unit) {
    let header = unit.kind === 'data-decl' ? unit.header : unit.header.split('=')[0];
    header = header.replace(/;\s*$/, '').replace(/\s+$/, '');
    if (/\[\s*\]\s*$/.test(header)) {
        // the size must be known by the other files (countof)
        const count = initializerItemCount(unit);
        if (count !== null) {
            header = header.replace(/\[\s*\]\s*$/, `[${count}]`);
        }
    }
    const lines = header.split('\n').map(l => l.replace(/\s+$/, '')).filter(l => l.trim() !== '');
    return 'extern ' + lines.join('\n').replace(/\bstatic\s+/, '').replace(/^\s+/, '') + ';';
}

function tentativeDeclarationOf(unit) {
    let header = unit.header.split('=')[0].replace(/;\s*$/, '').replace(/\s+$/, '');
    const lines = header.split('\n').map(l => l.replace(/\s+$/, '')).filter(l => l.trim() !== '');
    return lines.join('\n') + ';';
}

function removeStatic(unit) {
    // the first 'static' of the code part (comments before it are untouched)
    const codeOffset = unit.code.search(/\bstatic\b/);
    if (codeOffset < 0) {
        throw new Error(`no 'static' to remove in ${unit.names[0]}`);
    }
    return unit.text.slice(0, codeOffset) + unit.text.slice(codeOffset).replace(/^static\s+/, '');
}

// ---------------------------------------------------------------------------
// Emission of #if contexts

function condKey(cond) {
    return JSON.stringify(cond);
}

function emitWithConditions(pieces) {
    // pieces: [{ text, cond }] in output order
    const out = [];
    let index = 0;
    while (index < pieces.length) {
        const cond = pieces[index].cond;
        const key = condKey(cond);
        const group = [];
        while (index < pieces.length && condKey(pieces[index].cond) === key) {
            group.push(pieces[index].text);
            index++;
        }
        for (const frame of cond) {
            out.push(frame.join('\n'));
        }
        out.push(group.join('\n'));
        for (let k = 0; k < cond.length; k++) {
            out.push('#endif');
        }
    }
    return out.join('\n');
}

// ---------------------------------------------------------------------------
// Main

const source = fs.readFileSync(inputPath, 'utf8').replace(/\r\n/g, '\n');
const parsed = parseUnits(source, 1);
if (parsed.warnings.length) {
    console.log('warnings:\n  ' + parsed.warnings.join('\n  '));
}
const sourceLines = source.split('\n');
let units = parsed.units;

// 2. patches
for (const patch of config.patches) {
    const findUnit = name => units.find(u => u.kind === 'func' && u.names.includes(name))
        || units.find(u => u.names.includes(name));
    patch.apply({ units, parseUnits, analyzeUnit, findUnit });
}
units = units.filter(u => !u.removed);

// macros defined inside function bodies: a copy goes to a header if another
// file uses them (an identical redefinition is allowed in C)
for (const unit of [...units]) {
    if (unit.kind !== 'func') continue;
    const lines = unit.text.split('\n');
    for (let k = 0; k < lines.length; k++) {
        if (!/^\s*#\s*define\b/.test(lines[k])) continue;
        let end = k;
        while (lines[end].trimEnd().endsWith('\\') && end + 1 < lines.length) end++;
        const macro = {
            kind: 'macro', text: lines.slice(k, end + 1).join('\n'), cond: unit.cond,
            startLine: unit.startLine, sortKey: unit.sortKey + 0.0001,
            embedded: true, embeddedIn: unit,
        };
        analyzeUnit(macro);
        units.push(macro);
    }
}
units.sort((a, b) => a.sortKey - b.sortKey);

// 3. locations
function fileForLine(line) {
    let file = null;
    for (const [first, name] of config.fileRanges) {
        if (line >= first) file = name;
    }
    return file;
}

for (const unit of units) {
    const isPrelude = unit.startLine < config.preludeEnd && !unit.synthetic;
    unit.inPrelude = isPrelude;
    if (unit.forcedFile) {
        unit.loc = unit.forcedFile;
    } else if (isPrelude) {
        unit.loc = 'H';
    } else {
        unit.loc = fileForLine(unit.startLine);
    }
    const override = unit.names.map(n => config.overrides[n]).find(Boolean);
    if (override) {
        unit.loc = override;
    }
}
for (const unit of units) {
    if (unit.embedded) unit.loc = unit.embeddedIn.loc;
}

// definitions by name
const definitions = new Map();
function addDefinition(name, unit) {
    if (!definitions.has(name)) definitions.set(name, []);
    definitions.get(name).push(unit);
}
// a macro that is #undef'd somewhere (even inside a function) is local
const undefinedMacros = new Set([...source.matchAll(/^\s*#\s*undef\s+([A-Za-z_]\w*)/gm)].map(m => m[1]));
for (const unit of units) {
    const isDefinition = ['func', 'data', 'type'].includes(unit.kind)
        || (unit.kind === 'macro' && !undefinedMacros.has(unit.names[0]));
    if (isDefinition) {
        for (const name of unit.names) addDefinition(name, unit);
    }
}
// tentative data declarations that have a real definition are forward declarations
for (const unit of units) {
    if (unit.kind === 'data-decl') {
        const realDefinition = (definitions.get(unit.names[0]) || []).some(d => d.kind === 'data');
        if (realDefinition) {
            unit.dropped = true;
        } else {
            unit.kind = 'data';
            addDefinition(unit.names[0], unit);
        }
    }
}
// prototypes of functions defined in quickjs.c are regenerated
for (const unit of units) {
    if (unit.kind === 'proto' && definitions.has(unit.names[0])) {
        unit.dropped = true;
    }
}

const activeUnits = () => units.filter(u => !u.dropped);

// prelude data goes to the file of its first user
for (const unit of activeUnits()) {
    if (unit.inPrelude && unit.kind === 'data') {
        const firstUser = activeUnits().find(u => !u.inPrelude && u.refs.has(unit.names[0]));
        if (firstUser) {
            unit.loc = firstUser.loc;
            unit.inPrelude = false;
        }
    }
}

// 4. sharing analysis (fixpoint)

// functions already declared by the public header quickjs.h
const publicHeader = fs.readFileSync(path.join(path.dirname(inputPath), 'quickjs.h'), 'utf8');
const publicFunctions = new Set([...stripCommentsAndStrings(publicHeader).matchAll(/([A-Za-z_]\w*)\s*\(/g)].map(m => m[1]));

const virtualUnits = [];
const sharedFunctions = new Set();
const sharedData = new Set();

function share(definition) {
    const movesToHeader = definition.kind === 'type' || definition.kind === 'macro'
        || (definition.kind === 'func' && definition.isInline);
    if (movesToHeader) {
        definition.movedFrom = definition.loc;
        definition.loc = 'H';
        return true;
    }
    if (definition.kind === 'func' && !sharedFunctions.has(definition)) {
        sharedFunctions.add(definition);
        const declaredByPublicHeader = !definition.isStatic && publicFunctions.has(definition.names[0]);
        if (!declaredByPublicHeader) {
            virtualUnits.push(makeVirtual(definition, prototypeOf(definition, false)));
        }
        return true;
    }
    if (definition.kind === 'data' && !sharedData.has(definition)) {
        sharedData.add(definition);
        virtualUnits.push(makeVirtual(definition, externDeclarationOf(definition)));
        return true;
    }
    return false;
}

function makeVirtual(definition, text) {
    const unit = {
        kind: 'decl', names: [], text, cond: definition.cond, sortKey: definition.sortKey,
        loc: 'H', declares: definition,
    };
    unit.code = stripCommentsAndStrings(text);
    unit.refs = new Set([...identifiersOf(unit.code)].filter(id => !definition.names.includes(id)));
    for (const frame of unit.cond) {
        for (const directive of frame) {
            for (const id of identifiersOf(stripCommentsAndStrings(directive).replace(/^\s*#\s*\w+/, ''))) {
                if (id !== 'defined') unit.refs.add(id);
            }
        }
    }
    return unit;
}

let changed = true;
while (changed) {
    changed = false;
    for (const unit of activeUnits().concat(virtualUnits)) {
        for (const name of unit.refs) {
            for (const definition of definitions.get(name) || []) {
                if (definition.dropped || definition.loc === 'H' || definition.loc === unit.loc) {
                    continue;
                }
                if (share(definition)) changed = true;
            }
        }
    }
}

// header units: internal header (H) or private compiler header (C)
const isCompilerFile = loc => typeof loc === 'string' && loc.startsWith('compiler/');
const headerOrigin = unit => unit.declares ? unit.declares.loc : unit.movedFrom;
const headerUnits = activeUnits().filter(u => u.loc === 'H' && !u.inPrelude).concat(virtualUnits);
const compilerCandidates = new Set(headerUnits.filter(u => isCompilerFile(headerOrigin(u))));

function referencers(target) {
    const names = target.declares ? target.declares.names : target.names;
    return activeUnits().concat(virtualUnits).filter(u => u !== target && u !== target.declares
        && names.some(n => u.refs.has(n)));
}
let promoted = true;
while (promoted) {
    promoted = false;
    for (const candidate of [...compilerCandidates]) {
        const usedOutsideCompiler = referencers(candidate).some(r => {
            const location = r.loc === 'H' ? (compilerCandidates.has(r) ? 'C' : 'H') : r.loc;
            return location === 'H' || (location !== 'C' && !isCompilerFile(location));
        });
        if (usedOutsideCompiler) {
            compilerCandidates.delete(candidate);
            promoted = true;
        }
    }
}
for (const unit of headerUnits) {
    unit.header_ = compilerCandidates.has(unit) ? 'C' : 'H';
}

// ---------------------------------------------------------------------------
// 5. Writing

const FOLDER_HEADERS = { core: 'core/core.h', bytecode: 'bytecode/bytecode.h', builtins: 'builtins/builtins.h' };
const files = new Map();
function addToFile(file, unit) {
    if (!files.has(file)) files.set(file, []);
    files.get(file).push(unit);
}
for (const unit of activeUnits()) {
    // an embedded macro stays inside its function, it is only copied to a header
    if (unit.loc !== 'H' && !unit.embedded) addToFile(unit.loc, unit);
}

function write(relativePath, content) {
    const fullPath = path.join(outputDir, relativePath);
    fs.mkdirSync(path.dirname(fullPath), { recursive: true });
    fs.writeFileSync(fullPath, content.replace(/\n{3,}/g, '\n\n').trimEnd() + '\n');
}

function lineRangesOf(fileUnits) {
    const lines = fileUnits.filter(u => !u.synthetic).map(u => u.startLine).sort((a, b) => a - b);
    const ranges = [];
    for (const line of lines) {
        const last = ranges[ranges.length - 1];
        const contiguousGap = 400;
        if (last && line - last[1] < contiguousGap) last[1] = line; else ranges.push([line, line]);
    }
    return ranges.map(([a, b]) => `${a}-${b}`).join(', ');
}

const BANNER = (title, detail) => `/*
 * QuickJS - ${title}
 *
 * Generated by splitting quickjs.c (QuickJS ${config.version}, Fabrice Bellard
 * and Charlie Gordon, MIT license).${detail ? '\n * ' + detail : ''}
 */
`;

const sorted = list => [...list].sort((a, b) => a.sortKey - b.sortKey);
const textOfUnit = unit => {
    const isShared = (unit.kind === 'func' && sharedFunctions.has(unit)) || (unit.kind === 'data' && sharedData.has(unit));
    if (isShared && unit.isStatic) {
        return removeStatic(unit);
    }
    return unit.text;
};

// .c files
for (const [file, fileUnits] of files) {
    const ordered = sorted(fileUnits);
    const isCompiler = isCompilerFile(file);
    const position = new Map(ordered.map((u, k) => [u, k]));
    // forward declarations of local static functions and data used before
    // their definition: placed just before the first use
    const forwardBefore = new Map();
    for (const unit of ordered) {
        const isLocalFunction = unit.kind === 'func' && !sharedFunctions.has(unit);
        const isLocalData = unit.kind === 'data' && !sharedData.has(unit);
        if (!isLocalFunction && !isLocalData) continue;
        const firstUser = ordered.find(u => position.get(u) < position.get(unit) && unit.names.some(n => u.refs.has(n)));
        if (firstUser) {
            const text = isLocalFunction ? prototypeOf(unit, true) : tentativeDeclarationOf(unit);
            if (!forwardBefore.has(firstUser)) forwardBefore.set(firstUser, []);
            forwardBefore.get(firstUser).push({ text, cond: unit.cond });
        }
    }
    const pieces = [];
    for (const unit of ordered) {
        for (const declaration of forwardBefore.get(unit) || []) {
            pieces.push(declaration);
        }
        pieces.push({ text: textOfUnit(unit), cond: unit.cond });
    }
    let content = BANNER(file, `Lines of quickjs.c: ${lineRangesOf(ordered)}`);
    content += '\n#include "quickjs-internal.h"\n';
    if (isCompiler) content += '#include "compiler/compiler.h"\n';
    content += '\n' + emitWithConditions(pieces) + '\n';
    write(file, content);
}

// folder headers
for (const [folder, headerPath] of Object.entries(FOLDER_HEADERS)) {
    const declarations = sorted(virtualUnits.filter(u => u.header_ === 'H' && u.declares.loc.startsWith(folder + '/')));
    const guard = 'QUICKJS_' + folder.toUpperCase() + '_H';
    let content = BANNER(`${folder}/ prototypes and shared data`, 'Included by quickjs-internal.h.');
    content += `\n#ifndef ${guard}\n#define ${guard}\n\n`;
    content += emitWithConditions(declarations.map(u => ({ text: u.text, cond: u.cond })));
    content += `\n\n#endif /* ${guard} */\n`;
    write(headerPath, content);
}

// quickjs-internal.h
const droppedPreludeLines = new Set();
for (const unit of units) {
    const leavesPrelude = unit.startLine < config.preludeEnd && !unit.synthetic && (unit.dropped || !unit.inPrelude);
    if (leavesPrelude) unit.lineIndices.forEach(k => droppedPreludeLines.add(k));
}
const preludeText = sourceLines.slice(0, config.preludeEnd - 1)
    .filter((_, k) => !droppedPreludeLines.has(k)).join('\n');

const internalMoved = sorted(headerUnits.filter(u => u.header_ === 'H' && !u.declares && u.kind !== 'func'));
const internalInline = sorted(headerUnits.filter(u => u.header_ === 'H' && u.kind === 'func'));
const compilerDeclsOutside = sorted(virtualUnits.filter(u => u.header_ === 'H' && isCompilerFile(u.declares.loc)));

let internal = BANNER('internal declarations', 'Types, macros and inline helpers shared by all the engine files.');
internal += '\n#ifndef QUICKJS_INTERNAL_H\n#define QUICKJS_INTERNAL_H\n\n';
internal += preludeText.replace(/^\/\*[\s\S]*?\*\/\s*/, '') + '\n';
internal += '\n/* ---- macros and types shared between files ---- */\n\n';
internal += emitWithConditions(internalMoved.map(u => ({ text: u.text, cond: u.cond }))) + '\n';
internal += '\n/* ---- prototypes and shared data ---- */\n\n';
internal += Object.values(FOLDER_HEADERS).map(h => `#include "${h}"`).join('\n') + '\n';
if (compilerDeclsOutside.length) {
    internal += '\n/* compiler functions used outside of compiler/ */\n';
    internal += emitWithConditions(compilerDeclsOutside.map(u => ({ text: u.text, cond: u.cond }))) + '\n';
}
internal += '\n/* ---- inline functions shared between files ---- */\n\n';
internal += emitWithConditions(internalInline.map(u => ({ text: prototypeOf(u, true), cond: u.cond }))) + '\n\n';
internal += emitWithConditions(internalInline.map(u => ({ text: u.text, cond: u.cond }))) + '\n';
internal += '\n#endif /* QUICKJS_INTERNAL_H */\n';
write('quickjs-internal.h', internal);

// compiler/compiler.h
const compilerMoved = sorted(headerUnits.filter(u => u.header_ === 'C' && !u.declares && u.kind !== 'func'));
const compilerDecls = sorted(headerUnits.filter(u => u.header_ === 'C' && u.declares));
const compilerInline = sorted(headerUnits.filter(u => u.header_ === 'C' && u.kind === 'func'));
let compiler = BANNER('compiler private declarations', 'Only included by the files of compiler/.');
compiler += '\n#ifndef QUICKJS_COMPILER_H\n#define QUICKJS_COMPILER_H\n\n';
compiler += emitWithConditions(compilerMoved.map(u => ({ text: u.text, cond: u.cond }))) + '\n\n';
compiler += emitWithConditions(compilerDecls.map(u => ({ text: u.text, cond: u.cond }))) + '\n\n';
compiler += emitWithConditions(compilerInline.map(u => ({ text: prototypeOf(u, true), cond: u.cond }))) + '\n\n';
compiler += emitWithConditions(compilerInline.map(u => ({ text: u.text, cond: u.cond }))) + '\n';
compiler += '\n#endif /* QUICKJS_COMPILER_H */\n';
write('compiler/compiler.h', compiler);

// report
const report = {
    files: [...files.entries()].map(([f, us]) => ({ file: f, units: us.length })),
    sharedFunctions: sharedFunctions.size,
    sharedData: sharedData.size,
    movedToInternal: internalMoved.length + internalInline.length,
    movedToCompiler: compilerMoved.length + compilerInline.length,
    compilerFunctionsUsedOutside: compilerDeclsOutside.map(u => u.declares.names[0]),
    unassigned: activeUnits().filter(u => !u.loc).map(u => u.startLine),
};
console.log(JSON.stringify({ ...report, files: report.files.length }, null, 1));
