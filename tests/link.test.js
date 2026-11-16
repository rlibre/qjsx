/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tests of the link of a package (sys/compiler link(), host/js/build.js):
// one table of names for all the modules, bytecode written with their
// numbers, the entry names.bin of the package and its place in the archive.
// Run with: bin/Debug/qjsx run -A tests/link.test.js
import * as fs from "sys/fs";
import { compile, link, bytecodeVersion, engineNames } from "sys/compiler";
import { build, NAMES_ENTRY } from "qjsx:build";
import { readZip } from "qjsx:zip";
import { reducePackage } from "qjsx:deploy";
import { check, equal, report } from "./assert.js";

const FIXED_NAMES_MARK = 0x80;      // first byte of a linked bytecode: the version with this bit
const NAMES_MAGIC = "QJSN";

function compileModule(name, source) {
    return compile(source, name, { module: true, strip: "all" }).bytecode;
}

function contains(bytes, text) {
    const needle = new TextEncoder().encode(text);
    for (let i = 0; i + needle.length <= bytes.length; i++) {
        let k = 0;
        while (k < needle.length && bytes[i + k] === needle[k]) {
            k++;
        }
        if (k === needle.length) {
            return true;
        }
    }
    return false;
}

// --- two modules that share names
const first = compileModule("src/first.js",
    'export let sharedCounter = 0; export function bumpShared(amount) { sharedCounter += amount; return { ownProperty: amount }; }');
const second = compileModule("src/second.js",
    'import { bumpShared, sharedCounter } from "./first.js"; export const result = bumpShared(2).ownProperty + sharedCounter + [1].length;');
const linked = link([first, second]);

equal(linked.modules.length, 2, "as many modules as given");
equal(first[0], bytecodeVersion, "a compiled module starts with the bytecode version");
equal(linked.modules[0][0], bytecodeVersion | FIXED_NAMES_MARK, "a linked module has its own first byte");
check(linked.modules[0].length < first.length && linked.modules[1].length < second.length, "the linked modules no longer hold their names");
check(!contains(linked.modules[0], "sharedCounter") && !contains(linked.modules[1], "bumpShared"), "no name in the linked bytecode");

// --- the image of the names: the names of the program once, not the engine's
equal(new TextDecoder().decode(linked.names.subarray(0, 4)), NAMES_MAGIC, "the image of the names starts with its mark");
for (const name of ["sharedCounter", "bumpShared", "ownProperty", "src/first.js", "result"]) {
    check(contains(linked.names, name), `the names hold '${name}'`);
}
check(!contains(linked.names, "length") && !contains(linked.names, "prototype"), "the names of the engine are not in the image");
const view = new DataView(linked.names.buffer, linked.names.byteOffset, linked.names.byteLength);
equal(view.getUint32(4, true), engineNames, "the image names the engine it was made for");
equal(view.getUint32(20, true), linked.names.length, "the image gives its size");
equal(linked.names.length % 8, 0, "the image is a multiple of 8 bytes");

// --- wrong input
let problem = "";
try {
    link([new Uint8Array([1, 2, 3])]);
} catch (error) {
    problem = error.message;
}
check(problem.includes("not bytecode"), "what is not bytecode is refused");

// --- a package: the names are an entry, aligned in the archive, and kept in
// the package sent to a device
const DEMO = "tests/fixtures/demo";
const built = build(DEMO);
const archive = fs.readFileSync(built.file);
const entries = readZip(archive);
check(entries.has(NAMES_ENTRY), "the package has the entry of its names");
const manifest = JSON.parse(new TextDecoder().decode(entries.get("manifest.json")));
equal(`${manifest.names?.entry},${manifest.names?.engine}`, `${NAMES_ENTRY},${engineNames}`, "the manifest names the entry and the engine");

function offsetIn(whole, part) {
    return part.byteOffset - whole.byteOffset;
}
equal(offsetIn(archive, entries.get(NAMES_ENTRY)) % 8, 0, "the names start at a multiple of 8 bytes in the package");

const reduced = reducePackage(archive);
const reducedEntries = readZip(reduced);
check(reducedEntries.has(NAMES_ENTRY), "the package sent to a device keeps the names");
equal(offsetIn(reduced, reducedEntries.get(NAMES_ENTRY)) % 8, 0, "aligned there too");
for (const script of manifest.scripts) {
    equal(entries.get(script.bytecode)[0], bytecodeVersion | FIXED_NAMES_MARK, `${script.path} is linked`);
}

report("link");
