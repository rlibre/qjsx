/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Assembles the archive of a release for one system: everything the tool
// needs next to it, in one zip.
//
//   node tools/release/assemble.mjs --system windows-x64 --tool bin/Release/qjsx.exe
//        --stdlib bin/Release/stdlib.qjslib --firmware out/firmware --output dist
//        [--idf-notices COPYRIGHT.rst]
//
//     --system    the name of the system in the name of the archive
//                 (windows-x64, linux-arm)
//     --tool      the qjsx executable built for that system
//     --stdlib    stdlib.qjslib (the same file for every system: bytecode and
//                 sources, made by the tool of the building machine)
//     --firmware  a folder of firmware folders, one per board, each made by
//                 tools/release/firmware.mjs
//     --output    where the archive is written
//     --idf-notices  optional: the file of ESP-IDF that lists what it brings
//                 and under which licenses (docs/en/COPYRIGHT.rst of the
//                 ESP-IDF that built the firmwares); without it the archive
//                 is made with a warning: not one to publish
//
// The archive, dist/qjsx-<version>-<system>.zip, holds one folder:
//   qjsx-<version>-<system>/
//     qjsx(.exe)                 the tool (executable for its system)
//     stdlib.qjslib              the standard library
//     boards/*.json              the boards of the digital twin
//     flashers/*.js              how "qjsx flash" writes each family of chips
//     firmware/<board>/...       the firmware of each board
//     LICENSE, THIRD-PARTY-NOTICES.md, CHANGELOG.md, README.md
//     licenses/*.txt             the licenses THIRD-PARTY-NOTICES.md refers to
//     licenses/ESP-IDF-COPYRIGHT.rst   the file given by --idf-notices
// The version is the one of common/runtime/version.h.
//
// The zip is written here (deflate of Node's zlib), with no tool of the
// machine: the same archive comes out on Windows and on a build machine of
// GitHub, and the executable keeps its permission for Linux.
import * as fs from "node:fs";
import * as path from "node:path";
import * as zlib from "node:zlib";
import { fileURLToPath } from "node:url";

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const VERSION_FILE = "common/runtime/version.h";

// What goes into every archive from the repository, besides the tool, the
// standard library and the firmwares.
const FOLDERS = [
    { folder: "boards", extension: ".json" },
    { folder: "flashers", extension: ".js" },
    { folder: "licenses", extension: ".txt" },
];
const IDF_NOTICES = "licenses/ESP-IDF-COPYRIGHT.rst";   // its name in the archive
const DOCUMENTS = ["LICENSE", "THIRD-PARTY-NOTICES.md", "CHANGELOG.md", "README.md"];

// Permissions of an entry, as Unix sees them.
const MODE_FILE = 0o100644;
const MODE_EXECUTABLE = 0o100755;

/**
 * Stops with a message.
 */
function fail(message) {
    console.error(`assemble: ${message}`);
    process.exit(1);
}

/**
 * The options of the command line: --name value pairs; those of 'names' are
 * required, those of 'optional' are not.
 */
function readOptions(names, optional) {
    const options = {};
    const words = process.argv.slice(2);

    for (let index = 0; index < words.length; index += 2) {
        const name = words[index].replace(/^--/, "");
        const known = names.includes(name) || optional.includes(name);
        if (!known || words[index + 1] === undefined) {
            const all = [...names, ...optional].map(item => `--${item}`).join(", ");
            fail(`unexpected '${words[index]}' (options: ${all}, each with a value)`);
        }
        options[name] = words[index + 1];
    }

    for (const name of names) {
        if (options[name] === undefined) {
            fail(`--${name} is missing`);
        }
    }

    return options;
}

/**
 * The version of qjsx, as common/runtime/version.h defines it.
 */
function qjsxVersion() {
    const text = fs.readFileSync(path.join(ROOT, VERSION_FILE), "utf8");
    const match = /^#define QJSX_VERSION "([^"]+)"$/m.exec(text);
    if (match === null) {
        fail(`${VERSION_FILE}: no version there`);
    }

    return match[1];
}

/**
 * Every file under 'folder', as paths relative to it with '/', sorted.
 */
function filesUnder(folder) {
    const found = [];
    for (const entry of fs.readdirSync(folder, { withFileTypes: true, recursive: true })) {
        if (entry.isFile()) {
            const full = path.join(entry.parentPath, entry.name);
            found.push(path.relative(folder, full).split(path.sep).join("/"));
        }
    }

    return found.sort();
}

// ### the zip ################################################################

const LOCAL_SIGNATURE = 0x04034b50;
const CENTRAL_SIGNATURE = 0x02014b50;
const END_SIGNATURE = 0x06054b50;
const VERSION_NEEDED = 20;              // deflate
const MADE_BY_UNIX = (3 << 8) | 20;     // the permissions below are Unix's
const METHOD_DEFLATE = 8;
const UTF8_NAMES = 0x0800;

/**
 * The date and time of an entry as a zip stores them (2 seconds a step).
 */
function dosDateTime(date) {
    const time = (date.getHours() << 11) | (date.getMinutes() << 5) | (date.getSeconds() >> 1);
    const day = ((date.getFullYear() - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate();
    return { time, day };
}

/**
 * A zip of 'entries' ({ name, bytes, mode }), each compressed: their local
 * records, the central directory, its end.
 */
function makeZip(entries) {
    const { time, day } = dosDateTime(new Date());
    const parts = [];
    const central = [];
    let offset = 0;

    for (const entry of entries) {
        // its name, its bytes compressed, their check
        const name = Buffer.from(entry.name, "utf8");
        const packed = zlib.deflateRawSync(entry.bytes, { level: 9 });
        const crc = zlib.crc32(entry.bytes);

        // its local record, then its bytes
        const local = Buffer.alloc(30);
        local.writeUInt32LE(LOCAL_SIGNATURE, 0);
        local.writeUInt16LE(VERSION_NEEDED, 4);
        local.writeUInt16LE(UTF8_NAMES, 6);
        local.writeUInt16LE(METHOD_DEFLATE, 8);
        local.writeUInt16LE(time, 10);
        local.writeUInt16LE(day, 12);
        local.writeUInt32LE(crc, 14);
        local.writeUInt32LE(packed.length, 18);
        local.writeUInt32LE(entry.bytes.length, 22);
        local.writeUInt16LE(name.length, 26);
        parts.push(local, name, packed);

        // its record of the central directory: the same, its permissions,
        // where its local record starts
        const record = Buffer.alloc(46);
        record.writeUInt32LE(CENTRAL_SIGNATURE, 0);
        record.writeUInt16LE(MADE_BY_UNIX, 4);
        record.writeUInt16LE(VERSION_NEEDED, 6);
        record.writeUInt16LE(UTF8_NAMES, 8);
        record.writeUInt16LE(METHOD_DEFLATE, 10);
        record.writeUInt16LE(time, 12);
        record.writeUInt16LE(day, 14);
        record.writeUInt32LE(crc, 16);
        record.writeUInt32LE(packed.length, 20);
        record.writeUInt32LE(entry.bytes.length, 24);
        record.writeUInt16LE(name.length, 28);
        record.writeUInt32LE((entry.mode << 16) >>> 0, 38);
        record.writeUInt32LE(offset, 42);
        central.push(record, name);

        offset += local.length + name.length + packed.length;
    }

    // the end: how many entries, the size of the directory, where it starts
    const directory = Buffer.concat(central);
    const end = Buffer.alloc(22);
    end.writeUInt32LE(END_SIGNATURE, 0);
    end.writeUInt16LE(entries.length, 8);
    end.writeUInt16LE(entries.length, 10);
    end.writeUInt32LE(directory.length, 12);
    end.writeUInt32LE(offset, 16);

    return Buffer.concat([...parts, directory, end]);
}

// ### the archive ############################################################

const options = readOptions(["system", "tool", "stdlib", "firmware", "output"], ["idf-notices"]);
const version = qjsxVersion();
const top = `qjsx-${version}-${options.system}`;
const entries = [];

/**
 * Adds a file of the disk to the archive under 'name' (in its top folder).
 */
function add(name, file, mode = MODE_FILE) {
    if (!fs.existsSync(file)) {
        fail(`${file} is missing`);
    }

    entries.push({ name: `${top}/${name}`, bytes: fs.readFileSync(file), mode });
}

// the tool, under its name on that system, and the standard library
const toolName = options.system.startsWith("windows") ? "qjsx.exe" : "qjsx";
add(toolName, options.tool, MODE_EXECUTABLE);
add("stdlib.qjslib", options.stdlib);

// the boards and the flashers of the repository
for (const { folder, extension } of FOLDERS) {
    const names = fs.readdirSync(path.join(ROOT, folder)).filter(name => name.endsWith(extension)).sort();
    for (const name of names) {
        add(`${folder}/${name}`, path.join(ROOT, folder, name));
    }
}

// the firmwares: every board folder that has its description
const boards = fs.readdirSync(options.firmware).filter(name => fs.existsSync(path.join(options.firmware, name, "firmware.json"))).sort();
if (boards.length === 0) {
    fail(`${options.firmware}: no firmware folder there (made by tools/release/firmware.mjs)`);
}

for (const board of boards) {
    for (const file of filesUnder(path.join(options.firmware, board))) {
        add(`firmware/${board}/${file}`, path.join(options.firmware, board, file));
    }
}

// the documents
for (const document of DOCUMENTS) {
    add(document, path.join(ROOT, document));
}

// what ESP-IDF brings, as the ESP-IDF that built the firmwares lists it
if (options["idf-notices"] !== undefined) {
    add(IDF_NOTICES, options["idf-notices"]);
} else {
    console.warn(`assemble: warning: no --idf-notices: ${IDF_NOTICES} is not in the archive (not an archive to publish)`);
}

// written
fs.mkdirSync(options.output, { recursive: true });
const archive = path.join(options.output, `${top}.zip`);
const zip = makeZip(entries);
fs.writeFileSync(archive, zip);
console.log(`assemble: ${archive}: ${entries.length} files, ${boards.length} boards (${boards.join(", ")}), ${zip.length} bytes`);
