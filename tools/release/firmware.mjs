/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Makes the firmware folder of a board as a release delivers it, from the
// build folder of ESP-IDF: the files to write, side by side, and
// firmware.json, which "qjsx flash --board <board>" reads (host/js/flash.js).
//
//   node tools/release/firmware.mjs <build folder> <board> <output folder>
//   node tools/release/firmware.mjs ports/esp32/build-wt32-sc01 wt32-sc01 out/firmware/wt32-sc01
//
// What the build folder gives: flasher_args.json, which every ESP-IDF build
// writes (the files and their addresses, the chip, the size of the flash).
// The output folder is made, or emptied of a former firmware:
//   firmware.json   { "flasher": "esp32", "board", "version", "chip",
//                     "flashSize", "files": [{ "address", "file" }] }
//   bootloader.bin, partition-table.bin, qjsx.bin...   the files, by their
//                   names alone (two files of the same name are refused)
//
// Node, not qjsx: the firmwares are built on a machine that has no qjsx (the
// ESP-IDF image of a release build).
import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

const IDF_FILE = "flasher_args.json";       // what an ESP-IDF build writes
const FIRMWARE_FILE = "firmware.json";      // what qjsx flash reads
const FLASHER = "esp32";                    // the flasher of what ESP-IDF builds
const VERSION_FILE = "../../common/runtime/version.h";   // from this file

/**
 * Stops with a message.
 */
function fail(message) {
    console.error(`firmware: ${message}`);
    process.exit(1);
}

/**
 * The version of qjsx, as common/runtime/version.h defines it.
 */
function qjsxVersion() {
    const file = path.join(path.dirname(fileURLToPath(import.meta.url)), VERSION_FILE);
    const match = /^#define QJSX_VERSION "([^"]+)"$/m.exec(fs.readFileSync(file, "utf8"));
    if (match === null) {
        fail(`${file}: no version there`);
    }

    return match[1];
}

// --- the arguments
const [buildFolder, board, outputFolder] = process.argv.slice(2);
if (outputFolder === undefined) {
    fail("usage: node tools/release/firmware.mjs <build folder> <board> <output folder>");
}

// --- what the build wrote
const argsFile = path.join(buildFolder, IDF_FILE);
if (!fs.existsSync(argsFile)) {
    fail(`${argsFile} is missing: build the firmware of the board first`);
}

const args = JSON.parse(fs.readFileSync(argsFile, "utf8"));
const entries = Object.entries(args.flash_files ?? {});
if (entries.length === 0) {
    fail(`${argsFile}: no file to write`);
}

// --- the files, by their names alone, in the order of their addresses
entries.sort((a, b) => Number(a[0]) - Number(b[0]));
const files = entries.map(([address, source]) => ({ address, file: path.basename(source), source: path.join(buildFolder, source) }));
const names = new Set(files.map(entry => entry.file));
if (names.size !== files.length) {
    fail(`${argsFile}: two files have the same name`);
}

for (const entry of files) {
    if (!fs.existsSync(entry.source)) {
        fail(`${entry.source} is missing: the build is not complete`);
    }
}

// --- the folder, emptied, then the files and their description
fs.rmSync(outputFolder, { recursive: true, force: true });
fs.mkdirSync(outputFolder, { recursive: true });
for (const entry of files) {
    fs.copyFileSync(entry.source, path.join(outputFolder, entry.file));
}

const description = {
    flasher: FLASHER,
    board,
    version: qjsxVersion(),
    chip: args.extra_esptool_args?.chip,
    flashSize: args.flash_settings?.flash_size,
    files: files.map(entry => ({ address: entry.address, file: entry.file })),
};
fs.writeFileSync(path.join(outputFolder, FIRMWARE_FILE), `${JSON.stringify(description, null, 4)}\n`);

const total = files.reduce((sum, entry) => sum + fs.statSync(entry.source).size, 0);
console.log(`firmware: ${board} (${description.chip}), ${files.length} files, ${total} bytes -> ${outputFolder}`);
