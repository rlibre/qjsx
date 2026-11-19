/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// "qjsx flash": writes the firmware of a board through a serial port of the
// PC, without any tool of the chip's vendor.
//
// Two kinds of files, both NEXT TO THE EXECUTABLE (delivered with it, not
// inside it):
//   - flashers/<family>.js: how a family of chips is written (esp32.js: the
//     serial loader of the ROM of the ESP32s). A module that exports
//       async flash({ port, firmware, baudRate, log })
//     and imports nothing: it gets the open port and the files;
//   - firmware/<board>/: the files of the firmware of a board and
//     firmware.json, which says how to write them:
//       { "flasher": "esp32", "chip": "esp32p4", "flashSize": "16MB",
//         "files": [{ "address": "0x2000", "file": "bootloader.bin" }, ...] }
//
// A firmware folder may also be a build folder of ESP-IDF
// (ports/esp32/build-<board>): its flasher_args.json, which every build
// writes, gives the same facts.
//
//   import { flashFirmware, firmwareFolderOf } from "qjsx:flash";
//   await flashFirmware({ port: "COM32", folder, baudRate, log });
import * as fs from "sys/fs";
import { join, dirname } from "stdlib/path";
import { Serial } from "device/serial";

const FLASHERS_FOLDER = "flashers";        // next to the executable
const FIRMWARE_FOLDER = "firmware";        // next to the executable, a folder per board
const FIRMWARE_FILE = "firmware.json";     // the description of a firmware
const IDF_FILE = "flasher_args.json";      // the same facts, as an ESP-IDF build writes them
const IDF_FLASHER = "esp32";               // the flasher of what ESP-IDF builds
const OPEN_BAUD_RATE = 115200;             // the port is given to the flasher at this speed
const CLOSE_TIMEOUT = 1000;                // ms: the port is closed within about 100 ms

// The size of a flash as written ("16MB", "512KB") -> bytes.
const SIZE_UNITS = { KB: 1024, MB: 1024 * 1024 };

/**
 * The folder of the executable (process.argv[0] is its full path).
 */
function executableFolder() {
    return dirname(process.argv[0]);
}

/**
 * The firmware folder of a board delivered with the tool.
 */
export function firmwareFolderOf(board) {
    return join(executableFolder(), FIRMWARE_FOLDER, board);
}

/**
 * "16MB" -> bytes; undefined when absent. Throws for another text.
 */
function flashSizeOf(text, where) {
    if (text === undefined) {
        return undefined;
    }

    const match = /^(\d+)(KB|MB)$/.exec(text);
    if (match === null) {
        throw new Error(`${where}: flash size '${text}' is not a size such as "16MB"`);
    }

    return Number(match[1]) * SIZE_UNITS[match[2]];
}

/**
 * The address of a file as written ("0x2000") -> a number. Throws for
 * another text.
 */
function addressOf(text, where) {
    const address = Number(text);
    const valid = typeof text === "string" && Number.isInteger(address) && address >= 0;
    if (!valid) {
        throw new Error(`${where}: '${text}' is not an address such as "0x2000"`);
    }

    return address;
}

/**
 * The description of the firmware of 'folder', whichever file gives it:
 * { flasher, chip, flashSize: text | undefined, files: [{ address: text,
 * file }] }. Throws when the folder has neither file.
 */
function readDescription(folder) {
    // as delivered with the tool
    const ours = join(folder, FIRMWARE_FILE);
    if (fs.existsSync(ours)) {
        return { where: ours, ...JSON.parse(fs.readFileSync(ours, "utf8")) };
    }

    // as an ESP-IDF build writes it
    const idf = join(folder, IDF_FILE);
    if (fs.existsSync(idf)) {
        const args = JSON.parse(fs.readFileSync(idf, "utf8"));
        const files = Object.entries(args.flash_files ?? {}).map(([address, file]) => ({ address, file }));
        return { where: idf, flasher: IDF_FLASHER, chip: args.extra_esptool_args?.chip, flashSize: args.flash_settings?.flash_size, files };
    }

    throw new Error(`${folder}: no firmware there (neither ${FIRMWARE_FILE} nor ${IDF_FILE})`);
}

/**
 * Reads the firmware of 'folder': { flasher, chip, flashSize (bytes or
 * undefined), files: [{ name, address, bytes }] }, the files in the order
 * of their addresses. Throws when the description or a file is wrong.
 */
export function readFirmware(folder) {
    // its description
    const description = readDescription(folder);
    const where = description.where;
    const usable = typeof description.flasher === "string" && /^[a-z0-9_-]+$/.test(description.flasher)
        && Array.isArray(description.files) && description.files.length > 0;
    if (!usable) {
        throw new Error(`${where}: a firmware needs a "flasher" (a name) and "files"`);
    }

    // its files, read
    const files = description.files.map(entry => {
        const path = join(folder, String(entry.file));
        if (!fs.existsSync(path)) {
            throw new Error(`${where}: the file ${entry.file} is missing`);
        }

        return { name: String(entry.file), address: addressOf(entry.address, where), bytes: fs.readFileSync(path) };
    });
    files.sort((a, b) => a.address - b.address);

    return { flasher: description.flasher, chip: description.chip, flashSize: flashSizeOf(description.flashSize, where), files };
}

/**
 * Loads the flasher 'name' (flashers/<name>.js next to the executable).
 * Throws when it is missing or exports no flash().
 */
async function loadFlasher(name) {
    const path = join(executableFolder(), FLASHERS_FOLDER, `${name}.js`);
    if (!fs.existsSync(path)) {
        throw new Error(`no flasher '${name}' (${path} is missing)`);
    }

    const flasher = await import(path);
    if (typeof flasher.flash !== "function") {
        throw new Error(`${path}: a flasher exports a function flash()`);
    }

    return flasher;
}

/**
 * Closes a serial port and waits until it is (the next command may open it).
 */
async function closePort(port) {
    if (!port.isOpen()) {
        return;
    }

    await new Promise(resolve => {
        port.onClose(resolve);
        port.close();
        setTimeout(resolve, CLOSE_TIMEOUT);
    });
}

/**
 * Writes the firmware of 'folder' to the board behind the serial port
 * 'port' (its name: "COM32"), with the flasher its description names.
 * 'baudRate' (optional) is the speed of the transfer; 'log(text)' gets what
 * happens, a line at a time. Throws when it fails.
 */
export async function flashFirmware({ port, folder, baudRate, log }) {
    // the firmware and its flasher
    const firmware = readFirmware(folder);
    const flasher = await loadFlasher(firmware.flasher);
    const total = firmware.files.reduce((sum, file) => sum + file.bytes.length, 0);
    log(`firmware: ${firmware.chip ?? "chip not named"}, ${firmware.files.length} files, ${total} bytes (${folder})`);

    // the port, given to the flasher; closed whatever happens
    const serial = new Serial(port, { baudRate: OPEN_BAUD_RATE });
    try {
        await flasher.flash({ port: serial, firmware, baudRate, log });
    } finally {
        await closePort(serial);
    }
}
