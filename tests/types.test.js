/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Drift test of the typings (written by hand, a .d.ts next to each module:
// common/device/gpio_module.d.ts, lib/stdlib/src/path.d.ts...,
// common/runtime/modules/globals.d.ts; embedded as qjsx:types): what each
// module and the global scope declare must be what the runtime has, name for
// name, both ways:
//   - every native module (sys/..., device/...) and every module of the
//     standard library has a typing, and every typing a module;
//   - the exports of a module, the methods and properties of each exported
//     class (prototype and static), the members of each global namespace
//     (console, process);
//   - the global names that are not standard ECMAScript.
// Types themselves are not checked: only names (a renamed or removed export
// fails here). Runs in qjsx-tests, which registers the modules of a board.
// Run with: bin/Release/qjsx-tests run -A tests/types.test.js
import * as fs from "sys/fs";
import { check, report } from "./assert.js";
import { TYPINGS } from "qjsx:types";
import { nativeModuleCount, nativeModuleName } from "jsbind_test";

// The standard globals of ECMAScript (and of QuickJS): not declared by the
// typings, TypeScript's own library has them.
const STANDARD_GLOBALS = new Set([
    "globalThis", "Object", "Function", "Array", "Number", "parseFloat", "parseInt", "Infinity", "NaN",
    "undefined", "Boolean", "String", "Symbol", "Date", "Promise", "RegExp", "Error", "AggregateError",
    "EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", "URIError", "InternalError",
    "JSON", "Math", "Intl", "ArrayBuffer", "SharedArrayBuffer", "Uint8Array", "Int8Array", "Uint16Array",
    "Int16Array", "Uint32Array", "Int32Array", "Float16Array", "Float32Array", "Float64Array",
    "Uint8ClampedArray", "BigUint64Array", "BigInt64Array", "DataView", "Map", "BigInt", "Set", "WeakMap",
    "WeakSet", "Proxy", "Reflect", "WeakRef", "FinalizationRegistry", "Atomics", "decodeURI",
    "decodeURIComponent", "encodeURI", "encodeURIComponent", "escape", "unescape", "isFinite", "isNaN",
    "eval", "Iterator", "DisposableStack", "AsyncDisposableStack", "SuppressedError",
]);

// Names a class or a function has without declaring them.
const FUNCTION_OWN = new Set(["length", "name", "prototype", "caller", "arguments"]);

// ### Reading the typings ######################################################

const IDENTIFIER = "[A-Za-z_$][\\w$]*";
const MEMBER = new RegExp(`^ {8}((?:readonly |static |private |public |protected )*)(${IDENTIFIER})\\??\\s*[(:<]`);
const EXPORT_CLASS = new RegExp(`^ {4}export (?:declare )?(?:abstract )?class (${IDENTIFIER})`);
const EXPORT_VALUE = new RegExp(`^ {4}export (?:declare )?(?:function\\*? |const |let |var )(${IDENTIFIER})`);
const EXPORT_LIST = /^ {4}export \{([^}]*)\}/;
const EXPORT_DEFAULT = /^ {4}export default /;
const GLOBAL_DECLARATION = new RegExp(`^declare (function|class|const|let|var|namespace) (${IDENTIFIER})`);
const NAMESPACE_MEMBER = new RegExp(`^ {4}(?:function|const|let|var|class) (${IDENTIFIER})`);

// The runtime names declared by a module typing: { module, exports: Set,
// classes: Map name -> { members: Set, statics: Set } }.
function readModuleTyping(text) {
    const declared = { module: null, exports: new Set(), classes: new Map() };
    let currentClass = null;

    for (const line of text.split(/\r?\n/)) {
        // the module itself
        const moduleMatch = /^declare module "([^"]+)"/.exec(line);
        if (moduleMatch) {
            declared.module = moduleMatch[1];
            continue;
        }

        // the end of a class: its closing brace at 4 spaces
        if (currentClass !== null && /^ {4}\}/.test(line)) {
            currentClass = null;
            continue;
        }

        // a member of the current class (the constructor is the class itself);
        // a property ("name: T") is kept apart from the methods
        if (currentClass !== null) {
            const member = MEMBER.exec(line);
            if (member && member[2] !== "constructor" && !member[1].includes("private")) {
                const isStatic = member[1].includes("static");
                const isProperty = /^[^(<]*:/.test(line.slice(member.index + member[0].length - 1));
                const list = isStatic ? currentClass.statics : currentClass.members;
                list.add(member[2]);
                if (isProperty && !isStatic) {
                    currentClass.properties.add(member[2]);
                }
            }
            continue;
        }

        // the exports: classes, values, lists ("export { A, B as C } from ...")
        const exportedClass = EXPORT_CLASS.exec(line);
        if (exportedClass) {
            currentClass = { members: new Set(), statics: new Set(), properties: new Set() };
            declared.classes.set(exportedClass[1], currentClass);
            declared.exports.add(exportedClass[1]);
            continue;
        }
        const exportedValue = EXPORT_VALUE.exec(line);
        if (exportedValue) {
            declared.exports.add(exportedValue[1]);
            continue;
        }
        const exportedList = EXPORT_LIST.exec(line);
        if (exportedList && !/^ {4}export type /.test(line)) {
            for (const item of exportedList[1].split(",")) {
                const parts = item.trim().split(/\s+as\s+/);
                const name = parts[parts.length - 1].trim();
                if (name !== "" && !name.startsWith("type ")) {
                    declared.exports.add(name);
                }
            }
            continue;
        }
        if (EXPORT_DEFAULT.test(line)) {
            declared.exports.add("default");
        }
    }
    return declared;
}

// The global names declared by globals.d.ts: { names: Set, namespaces: Map
// name -> Set of members }.
function readGlobalTyping(text) {
    const declared = { names: new Set(), namespaces: new Map() };
    let currentNamespace = null;

    for (const line of text.split(/\r?\n/)) {
        // the end of a namespace: its closing brace at column 0
        if (currentNamespace !== null && /^\}/.test(line)) {
            currentNamespace = null;
            continue;
        }
        if (currentNamespace !== null) {
            const member = NAMESPACE_MEMBER.exec(line);
            if (member) {
                currentNamespace.add(member[1]);
            }
            continue;
        }

        // a global: function, class, variable or namespace (a plain object)
        const global = GLOBAL_DECLARATION.exec(line);
        if (global) {
            declared.names.add(global[2]);
            if (global[1] === "namespace") {
                currentNamespace = new Set();
                declared.namespaces.set(global[2], currentNamespace);
            }
        }
    }
    return declared;
}

// ### Comparing ################################################################

// Checks that 'actual' and 'declared' (iterables of names) hold the same
// names; 'what' says where, in the failure messages.
function sameNames(actual, declared, what) {
    const actualSet = new Set(actual);
    const declaredSet = new Set(declared);
    for (const name of actualSet) {
        check(declaredSet.has(name), `${what}: '${name}' exists but is not declared in the typings`);
    }
    for (const name of declaredSet) {
        check(actualSet.has(name), `${what}: '${name}' is declared in the typings but does not exist`);
    }
}

// The own string-keyed names of 'object', but those in 'ignored'.
function ownNames(object, ignored) {
    return Object.getOwnPropertyNames(object).filter(name => !ignored.has(name));
}

// Compares the module 'name' (imported) with its typing.
async function checkModule(name, declared) {
    let namespace;
    try {
        namespace = await import(name);
    } catch (error) {
        check(false, `${name}: cannot be imported (${error.message})`);
        return;
    }

    // its exports
    sameNames(Object.keys(namespace), declared.exports, `${name} exports`);

    // each exported class: prototype and static names. A native class has its
    // properties on the prototype (accessors of jsbind); a class written in JS
    // (standard library) may set them on each instance, in its constructor:
    // those cannot be seen without making an object
    const isNative = name.startsWith("sys/") || name.startsWith("device/");
    for (const [className, members] of declared.classes) {
        const value = namespace[className];
        if (typeof value !== "function") {
            continue;
        }
        const prototypeNames = ownNames(value.prototype, new Set(["constructor"]));
        let expected = members.members;
        if (!isNative) {
            expected = [...members.members].filter(member => !members.properties.has(member) || prototypeNames.includes(member));
        }
        sameNames(prototypeNames, expected, `${name} ${className}.prototype`);
        sameNames(ownNames(value, FUNCTION_OWN), members.statics, `${name} ${className} (static)`);
    }
}

// Compares the global scope with globals.d.ts.
function checkGlobals(declared) {
    // names beyond standard ECMAScript
    const extra = Object.getOwnPropertyNames(globalThis).filter(name => !STANDARD_GLOBALS.has(name));
    sameNames(extra, [...declared.names].filter(name => !STANDARD_GLOBALS.has(name)), "globals");

    // the members of each plain object (console, process...)
    for (const [name, members] of declared.namespaces) {
        const object = globalThis[name];
        if (object === undefined || object === null) {
            continue;
        }
        // their properties are not enumerable (unlike Node's): own names
        sameNames(Object.getOwnPropertyNames(object), members, `globals ${name}`);
    }
}

// ### The test #################################################################

// the typings, by module name
const byModule = new Map();
let globals = null;
for (const typing of TYPINGS) {
    if (typing.name === "globals") {
        globals = readGlobalTyping(typing.text);
        continue;
    }
    byModule.set(typing.name, readModuleTyping(typing.text));
}
check(globals !== null, "common/runtime/modules/globals.d.ts exists");

// the modules that need a typing: the native ones of the API, and those of
// the standard library
const modules = [];
for (let index = 0; index < nativeModuleCount(); index++) {
    const name = nativeModuleName(index);
    if (name.startsWith("sys/") || name.startsWith("device/")) {
        modules.push(name);
    }
}
const stdlib = JSON.parse(fs.readFileSync("lib/stdlib/qjsx.json", "utf8"));
for (const name of Object.keys(stdlib.modules)) {
    modules.push(`stdlib/${name}`);
}
sameNames(modules, byModule.keys(), "modules with a typing");

// each module, then the global scope
for (const name of modules) {
    if (byModule.has(name)) {
        await checkModule(name, byModule.get(name));
    }
}
if (globals !== null) {
    checkGlobals(globals);
}

report("types");
