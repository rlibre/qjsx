/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tests of the imports of bytes, 'import x from "./x.bin" with { type:
// "bytes" }' (host/js/bytes.js, common/runtime/program.cpp): what 'qjsx build'
// puts in the package and in its manifest, the reduced package, the errors
// with their position, the build id. tests/run-tests.sh also runs the
// fixture from its sources and from its package.
// Run with: bin/Debug/qjsx run -A tests/bytes.test.js
import * as fs from "sys/fs";
import { join, dirname } from "stdlib/path";
import { sha256 } from "sys/crypto";
import { build } from "qjsx:build";
import { readZip, writeZip } from "qjsx:zip";
import { makeLibrary } from "qjsx:library";
import { reducePackage } from "qjsx:deploy";
import { hex } from "qjsx:util";
import { check, equal, throws, report } from "./assert.js";

const FIXTURE = "tests/fixtures/bytes";
const SCRATCH = "build/bytes-test";   // projects written by this test (git-ignored)
const GLYPHS_ENTRY = "bytes/src/fonts/glyphs.bin";
const EMPTY_ENTRY = "bytes/src/empty.bin";
const decoder = new TextDecoder();

// Offset of 'part' (a subarray of 'whole', as readZip() gives the entries)
// in 'whole'.
function offsetIn(whole, part) {
    return part.byteOffset - whole.byteOffset;
}

// True if the two arrays hold the same bytes.
function sameBytes(a, b) {
    return a.length === b.length && a.every((byte, index) => byte === b[index]);
}

// The manifest of a package (its entries, from readZip()).
function manifestOf(entries) {
    return JSON.parse(decoder.decode(entries.get("manifest.json")));
}

// Writes the files of a project in SCRATCH/<name> (path in the project ->
// text or bytes), qjsx.json first ('project'). Returns its folder.
function writeFiles(name, project, files) {
    const directory = join(SCRATCH, name);
    fs.rmSync(directory, { recursive: true, force: true });
    const allFiles = { "qjsx.json": JSON.stringify(project), ...files };

    for (const [path, content] of Object.entries(allFiles)) {
        const file = join(directory, path);
        fs.mkdirSync(dirname(file), { recursive: true });
        fs.writeFileSync(file, content);
    }

    return directory;
}

// Writes a program project in SCRATCH/<name>: qjsx.json ('settings' added to
// it), then 'files'. Returns its folder.
function writeProject(name, files, settings = {}) {
    const project = { name, main: "src/main.js", sources: ["src/**/*.js"], keep: 1, ...settings };
    return writeFiles(name, project, files);
}

// Writes a library project in SCRATCH/<name> ('modules': module -> source
// file), then 'files'. Returns its folder.
function writeLibraryProject(name, modules, files) {
    return writeFiles(name, { name, version: "1.0.0", modules }, files);
}

// The names of the entries "bytes/..." of a package or a library.
function bytesEntries(entries) {
    return [...entries.keys()].filter(name => name.startsWith("bytes/")).sort();
}

// --- the package of the fixture: the files imported, as they are, aligned
const built = build(FIXTURE);
const archive = fs.readFileSync(built.file);
const entries = readZip(archive);
const glyphsFile = fs.readFileSync(join(FIXTURE, "src/fonts/glyphs.bin"));

check(entries.has(GLYPHS_ENTRY), "the package has the file imported as bytes");
check(sameBytes(entries.get(GLYPHS_ENTRY), glyphsFile), "with the exact bytes of the file");
equal(offsetIn(archive, entries.get(GLYPHS_ENTRY)) % 8, 0, "the file starts at a multiple of 8 bytes in the package");
equal(entries.get(EMPTY_ENTRY)?.length, 0, "an empty file is an empty entry");
equal(offsetIn(archive, entries.get(EMPTY_ENTRY)) % 8, 0, "aligned too");
check(![...entries.keys()].some(name => name.includes("unused.bin")), "a file never imported is not in the package");
check(![...entries.keys()].some(name => name.startsWith("bytecode/") && name.includes(".bin")), "a file of bytes is not compiled");

// --- its manifest lists them: path, size, hash
const manifest = manifestOf(entries);
const listed = (manifest.bytes ?? []).map(record => `${record.path}:${record.size}:${record.hash}`).sort();
const expected = [
    `src/empty.bin:0:${hex(sha256(new Uint8Array(0)))}`,
    `src/fonts/glyphs.bin:300:${hex(sha256(glyphsFile))}`,
];
equal(listed.join(), expected.join(), "the manifest lists each file imported as bytes once, with its size and hash");
equal(manifest.scripts.map(script => script.path).sort().join(), "src/lib/describe.js,src/main.js", "the files are not scripts");

// --- the package sent to a device keeps them, aligned, and the manifest's list
const reduced = reducePackage(archive);
const reducedEntries = readZip(reduced);
check(reducedEntries.has(GLYPHS_ENTRY) && reducedEntries.has(EMPTY_ENTRY), "the reduced package keeps the files");
check(sameBytes(reducedEntries.get(GLYPHS_ENTRY), glyphsFile), "with their bytes");
equal(offsetIn(reduced, reducedEntries.get(GLYPHS_ENTRY)) % 8, 0, "aligned there too");
equal(manifestOf(reducedEntries).bytes?.length, 2, "its manifest keeps the list");
check(![...reducedEntries.keys()].some(name => name.startsWith("src/")), "the sources stay on the PC");

// --- the build id covers the content of the files
{
    const glyphs = new Uint8Array(glyphsFile);
    const files = {
        "src/main.js": 'import glyphs from "./glyphs.bin" with { type: "bytes" };\nconsole.log(glyphs.length);\n',
        "src/glyphs.bin": glyphs,
    };
    const directory = writeProject("buildid", files);
    const first = build(directory).buildId;
    equal(build(directory).buildId, first, "the same files give the same build id");

    glyphs[100] ^= 0xff;
    fs.writeFileSync(join(directory, "src/glyphs.bin"), glyphs);
    const changed = build(directory);
    check(changed.buildId !== first, "a changed file changes the build id");
    check(sameBytes(readZip(fs.readFileSync(changed.file)).get("bytes/src/glyphs.bin"), glyphs), "the package holds the new bytes");
}

// --- errors, at the position of the module's string
const ERRORS = [
    {
        source: 'import data from "./data.bin" with { type: "json" };\n',
        pattern: /^Error: src\/main\.js:1:18: unsupported import type 'json'/,
        message: "another type is refused at its position",
    },
    {
        source: 'const a = 1;\nimport data from "./data.bin" with { type: "bytes", mode: "raw" };\n',
        pattern: /^Error: src\/main\.js:2:18: unsupported import attribute 'mode'/,
        message: "another attribute is refused",
    },
    {
        source: 'import data from "./missing.bin" with { type: "bytes" };\n',
        pattern: /^Error: src\/main\.js:1:18: no file 'src\/missing\.bin' for this import of bytes$/,
        message: "a missing file is refused at its position",
    },
    {
        source: 'import { first } from "./data.bin" with { type: "bytes" };\n',
        pattern: /^Error: src\/main\.js:1:10: 'first' is not exported by src\/data\.bin$/,
        message: "a file of bytes exports 'default' only",
    },
    {
        source: 'import data from "./data.bin" with { type: "bytes" };\nimport "./data.bin";\n',
        pattern: /^Error: src\/main\.js:2:8: 'src\/data\.bin' is imported both as bytes and as a module$/,
        message: "a file is imported as bytes or as a module, not both",
    },
    {
        source: 'import data from "src/data.bin" with { type: "bytes" };\n',
        pattern: /^Error: src\/main\.js:1:18: an import of bytes names a file relative to its module/,
        message: "an import of bytes is relative",
    },
    {
        source: 'import data from "../../data.bin" with { type: "bytes" };\n',
        pattern: /^Error: src\/main\.js:1:18: '\.\.\/\.\.\/data\.bin' is outside the project$/,
        message: "a file outside the project is refused",
    },
];

ERRORS.forEach((error, index) => {
    const directory = writeProject(`error${index}`, { "src/main.js": error.source, "src/data.bin": new Uint8Array([1, 2, 3]) });
    throws(() => build(directory), error.pattern, error.message);
});

// --- a library imports bytes: 'qjsx mklib' stores the files, aligned, and
// lists them; the imports of its modules say which are bytes
const SMALL = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13]);
const BIG = new Uint8Array(1000).map((_, index) => index * 7);
const SMALL_ENTRY = "bytes/fontlib/data/small.bin";
const BIG_ENTRY = "bytes/fontlib/data/big.bin";
const LIBRARY_MODULES = { "small": "src/small.js", "data/big": "src/data/big.js", "plain": "src/plain.js" };
const LIBRARY_FILES = {
    "src/small.js": 'import small from "./data/small.bin" with { type: "bytes" };\nexport default small;\n',
    "src/data/big.js": 'import big from "./big.bin" with { type: "bytes" };\nexport { big };\n',
    "src/plain.js": "export const plain = 1;\n",
    "src/data/small.bin": SMALL,
    "src/data/big.bin": BIG,
};
const libraryDirectory = writeLibraryProject("fontlib", LIBRARY_MODULES, LIBRARY_FILES);
const libraryFile = makeLibrary(libraryDirectory).file;
const libraryArchive = fs.readFileSync(libraryFile);
const libraryEntries = readZip(libraryArchive);
const libraryManifest = manifestOf(libraryEntries);

equal(bytesEntries(libraryEntries).join(), `${BIG_ENTRY},${SMALL_ENTRY}`, "the library holds each file, named by its module name");
check(sameBytes(libraryEntries.get(SMALL_ENTRY), SMALL) && sameBytes(libraryEntries.get(BIG_ENTRY), BIG), "with their exact bytes");
equal(offsetIn(libraryArchive, libraryEntries.get(SMALL_ENTRY)) % 8, 0, "aligned on 8 bytes in the library");
equal(offsetIn(libraryArchive, libraryEntries.get(BIG_ENTRY)) % 8, 0, "both of them");
equal(libraryManifest.format, 2, "a library with imports of bytes is of format 2");
{
    const listedFiles = libraryManifest.bytes.map(record => `${record.name}:${record.entry}:${record.size}:${record.hash}`).sort();
    const expectedFiles = [
        `fontlib/data/big.bin:${BIG_ENTRY}:1000:${hex(sha256(BIG))}`,
        `fontlib/data/small.bin:${SMALL_ENTRY}:13:${hex(sha256(SMALL))}`,
    ];
    equal(listedFiles.join(), expectedFiles.join(), "its manifest lists the files: name, entry, size, hash");

    const importsOf = name => JSON.stringify(libraryManifest.modules.find(module => module.name === name).imports);
    equal(importsOf("fontlib/small"), '[{"name":"fontlib/data/small.bin","bytes":true}]', "an import of bytes says so");
    equal(importsOf("fontlib/data/big"), '[{"name":"fontlib/data/big.bin","bytes":true}]', "resolved against the module name");
    equal(importsOf("fontlib/plain"), "[]", "a module without import");
}

// --- a program gets the files of the library modules it imports, and only
// those, from the library
const LIBRARY_PATH = `../fontlib/${libraryFile.slice(libraryDirectory.length + 1)}`;
const programFiles = { "src/main.js": 'import small from "fontlib/small";\nconsole.log(small.length);\n' };
const programDirectory = writeProject("usefontlib", programFiles, { libraries: [LIBRARY_PATH] });
const usingBuild = build(programDirectory);
const usingArchive = fs.readFileSync(usingBuild.file);
const usingEntries = readZip(usingArchive);
const usingManifest = manifestOf(usingEntries);

equal(bytesEntries(usingEntries).join(), SMALL_ENTRY, "the package holds the file of the imported module only");
check(sameBytes(usingEntries.get(SMALL_ENTRY), SMALL), "with its bytes, from the library");
equal(offsetIn(usingArchive, usingEntries.get(SMALL_ENTRY)) % 8, 0, "aligned in the package");
equal(JSON.stringify(usingManifest.bytes), JSON.stringify([{ path: "fontlib/data/small.bin", size: 13, hash: hex(sha256(SMALL)) }]),
    "the manifest of the package lists it by its module name");
check(bytesEntries(readZip(reducePackage(usingArchive))).includes(SMALL_ENTRY), "the reduced package keeps it");

// the build id covers the file imported, not the others of the library
{
    const changedBig = BIG.map(byte => byte ^ 0xff);
    writeLibraryProject("fontlib", LIBRARY_MODULES, { ...LIBRARY_FILES, "src/data/big.bin": changedBig });
    makeLibrary(libraryDirectory);
    equal(build(programDirectory).buildId, usingBuild.buildId, "a file of the library the program does not import changes nothing");

    const changedSmall = SMALL.map(byte => byte + 100);
    writeLibraryProject("fontlib", LIBRARY_MODULES, { ...LIBRARY_FILES, "src/data/small.bin": changedSmall });
    makeLibrary(libraryDirectory);
    const rebuilt = build(programDirectory);
    check(rebuilt.buildId !== usingBuild.buildId, "a changed file imported by the program changes the build id");
    check(sameBytes(readZip(fs.readFileSync(rebuilt.file)).get(SMALL_ENTRY), changedSmall), "the package holds its new bytes");
}

// --- a library of format 1 (before the imports of bytes: imports as names,
// no 'bytes') is still read
{
    const oldDirectory = writeLibraryProject("oldlib", { "plain": "src/plain.js", "user": "src/user.js" }, {
        "src/plain.js": "export const plain = 41;\n",
        "src/user.js": 'import { plain } from "./plain";\nexport const answer = plain + 1;\n',
    });
    const oldFile = makeLibrary(oldDirectory).file;
    const oldEntries = readZip(fs.readFileSync(oldFile));
    const oldManifest = manifestOf(oldEntries);
    oldManifest.format = 1;
    delete oldManifest.bytes;
    for (const module of oldManifest.modules) {
        module.imports = module.imports.map(imported => imported.name);
    }

    const rewritten = [...oldEntries].map(([name, data]) => ({ name, data }));
    rewritten[0].data = new TextEncoder().encode(JSON.stringify(oldManifest));
    fs.writeFileSync(oldFile, writeZip(rewritten));

    const oldUser = writeProject("useoldlib", { "src/main.js": 'import { answer } from "oldlib/user";\nconsole.log(answer);\n' },
        { libraries: [`../oldlib/${oldFile.slice(oldDirectory.length + 1)}`] });
    const oldScripts = manifestOf(readZip(fs.readFileSync(build(oldUser).file))).scripts.map(script => script.path);
    equal(oldScripts.sort().join(), "oldlib/plain,oldlib/user,src/main.js", "a program built with it follows its imports");
}

// --- errors of 'qjsx mklib': a file outside the library, a missing file
const LIBRARY_ERRORS = [
    {
        source: 'import data from "../outside.bin" with { type: "bytes" };\nexport default data;\n',
        pattern: /^Error: errlib0\/x:1:18: '\.\.\/outside\.bin' is the module 'outside\.bin', outside the library 'errlib0'$/,
        message: "a library file named outside the library is refused",
    },
    {
        source: 'import data from "./missing.bin" with { type: "bytes" };\nexport default data;\n',
        pattern: /^Error: errlib1\/x:1:18: no file 'src\/missing\.bin' for this import of bytes$/,
        message: "a missing file of a library is refused, with its path in the project",
    },
];

LIBRARY_ERRORS.forEach((error, index) => {
    const name = `errlib${index}`;
    const directory = writeLibraryProject(name, { "x": "src/x.js" }, { "src/x.js": error.source, "outside.bin": new Uint8Array([1]) });
    throws(() => makeLibrary(directory), error.pattern, error.message);
});

// --- a program that imports one font of the standard library gets that
// font only (tests/run-tests.sh runs it)
{
    const FONT_ENTRY = "bytes/stdlib/fonts/inter16.bin";
    const fontBuild = build("tests/fixtures/fonts");
    const fontArchive = fs.readFileSync(fontBuild.file);
    const fontEntries = readZip(fontArchive);
    const fontFile = fs.readFileSync("lib/stdlib/src/fonts/inter16.bin");

    equal(bytesEntries(fontEntries).join(), FONT_ENTRY, "a program importing stdlib/fonts/inter16 gets inter16.bin only");
    check(sameBytes(fontEntries.get(FONT_ENTRY), fontFile), "the font of lib/stdlib, as it is");
    equal(offsetIn(fontArchive, fontEntries.get(FONT_ENTRY)) % 8, 0, "aligned in the package");
    check(fontArchive.length < fontFile.length + 10000, `the package is the font and little more (${fontArchive.length} bytes)`);
}

report("bytes");
