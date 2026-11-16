/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The files of the data folder of a device, from the PC (FILE_* requests of
// the agent, qjsx:devicefiles: "qjsx device ls | read | write | rm | mkdir |
// mv", "qjsx deploy --data"), on a twin: content, folders, errors with their
// Node-like code, a write cut in the middle, the "data" folder of a project
// sent, then read by its program.
//
//   qjsx run -A tests/datafiles.test.js tcp:127.0.0.1:<agent port> <storage of the agent>
import * as fs from "sys/fs";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND, KEY, UPLOAD_CHUNK } from "qjsx:protocol";
import { listFiles, readFile, writeFile, removeFile, makeDirectory, renameFile, sendFolder } from "qjsx:devicefiles";
import { check, equal, rejects, report } from "./assert.js";

const TARGET = process.argv[2];
const STORAGE = process.argv[3];
const DATA = `${STORAGE}/data`;
const PROJECT = "tests/fixtures/datareader";
const WAIT_TIMEOUT = 8000;
const encoder = new TextEncoder();
const decoder = new TextDecoder();

// --- the program's output, line by line
const lines = [];

function onLog(stream, text) {
    for (const line of text.split("\n").filter(item => item !== "")) {
        lines.push(line);
    }
}

// Resolves true once the program has printed 'line', false after WAIT_TIMEOUT.
async function printed(line) {
    const end = Date.now() + WAIT_TIMEOUT;
    while (!lines.includes(line) && Date.now() < end) {
        await new Promise(resolve => setTimeout(resolve, 20));
    }
    return lines.includes(line);
}

// Checks that 'promise' is refused with the Node-like 'code'.
async function refused(promise, code, message) {
    try {
        await promise;
        check(false, `${message}: not refused`);
    } catch (error) {
        equal(error.code, code, message);
    }
}

// A clean data folder on the PC side of the twin
const device = await Device.open(TARGET, { onLog });
await device.hello();
await device.request(KIND.STOP);
fs.rmSync(DATA, { recursive: true, force: true });
fs.mkdirSync(DATA, { recursive: true });

// ### Files: written, read back, listed ########################################

await writeFile(device, "hello.txt", encoder.encode("hello"));
equal(decoder.decode(await readFile(device, "hello.txt")), "hello", "a small file read back");
equal(fs.readFileSync(`${DATA}/hello.txt`, "utf8"), "hello", "the file is in the data folder of the twin");

const big = new Uint8Array(UPLOAD_CHUNK * 2 + 123).map((_, index) => (index * 7) & 0xFF);
await writeFile(device, "big.bin", big);
const bigBack = await readFile(device, "big.bin");
check(bigBack.length === big.length && bigBack.every((byte, index) => byte === big[index]), "a file of several pieces read back whole");

await writeFile(device, "empty.txt", new Uint8Array(0));
equal((await readFile(device, "empty.txt")).length, 0, "an empty file");

await writeFile(device, "/été.txt", encoder.encode("accents"));
equal(decoder.decode(await readFile(device, "été.txt")), "accents", "a name with accents; a leading '/' is the data folder");

await makeDirectory(device, "logs");
await writeFile(device, "logs/./today.txt", encoder.encode("log"));
const names = (await listFiles(device, "")).map(entry => entry.name);
equal(names.join(","), "big.bin,empty.txt,hello.txt,logs,été.txt", "the entries, sorted");
const logs = (await listFiles(device, "/logs/")).find(entry => entry.name === "today.txt");
check(logs !== undefined && logs.size === 3 && !logs.isDirectory, "a file of a folder, with its size");
check((await listFiles(device, "")).find(entry => entry.name === "logs").isDirectory, "a folder is marked so");

// ### A write cut in the middle: the old content stays #########################

await device.request(KIND.FILE_WRITE_BEGIN, { [KEY.PATH]: "hello.txt", [KEY.SIZE]: 10 });
await device.request(KIND.FILE_WRITE_CHUNK, { [KEY.OFFSET]: 0, [KEY.DATA]: encoder.encode("HELLO") });
equal(decoder.decode(await readFile(device, "hello.txt")), "hello", "during a write, the old content is read");
check(!(await listFiles(device, "")).some(entry => entry.name.endsWith(".qjsx-part")), "the temporary file is not listed");
await rejects(device.request(KIND.FILE_WRITE_END), /incomplete|Invalid/, "a write cannot end before every byte");
await writeFile(device, "other.txt", encoder.encode("x"));
check(!fs.readdirSync(DATA).some(name => name.endsWith(".qjsx-part")), "a new write drops the unfinished one");
equal(decoder.decode(await readFile(device, "hello.txt")), "hello", "the unfinished write changed nothing");

// ### Rename, remove ###########################################################

await renameFile(device, "other.txt", "hello.txt");
equal(decoder.decode(await readFile(device, "hello.txt")), "x", "a file renamed onto a file replaces it");
await refused(renameFile(device, "hello.txt", "logs"), "EISDIR", "a file never replaces a folder");
await makeDirectory(device, "archive");
await refused(renameFile(device, "archive", "hello.txt"), "EEXIST", "a folder never replaces anything");
await renameFile(device, "archive", "old");
check((await listFiles(device, "")).some(entry => entry.name === "old" && entry.isDirectory), "a folder renamed");

await refused(removeFile(device, "logs"), "ENOTEMPTY", "a folder with files is not removed");
await removeFile(device, "logs/today.txt");
await removeFile(device, "logs");
await removeFile(device, "old");
check(!(await listFiles(device, "")).some(entry => entry.name === "logs"), "an empty folder removed");

// ### Refused ##################################################################

await refused(readFile(device, "nothing.txt"), "ENOENT", "a missing file");
await refused(readFile(device, "../agent.log"), "EINVAL", "'..' is refused");
await refused(listFiles(device, "a/../.."), "EINVAL", "'..' anywhere is refused");
await refused(writeFile(device, "C:/x.txt", encoder.encode("x")), "EINVAL", "a drive is refused");
await refused(removeFile(device, ""), "EINVAL", "the data folder itself is not removed");
await refused(makeDirectory(device, "big.bin"), "EEXIST", "mkdir on an existing name");
await makeDirectory(device, "folder");
await refused(readFile(device, "folder"), "EISDIR", "a folder is not read");
await refused(writeFile(device, "folder", encoder.encode("x")), "EISDIR", "a folder is not written over");
await refused(writeFile(device, "missing/x.txt", encoder.encode("x")), "ENOENT", "the parent folder must exist");

// ### What INFO says #############################################################

const info = await device.request(KIND.INFO);
check(info.get(KEY.DATA_USED) > big.length, "INFO: the bytes used by the data folder");
equal(info.get(KEY.DATA_TOTAL), undefined, "INFO: no partition size on the desktop");

// ### The data folder of a project, then its program ###########################

await writeFile(device, "kept.txt", encoder.encode("mine"));
await writeFile(device, "config.json", encoder.encode("old"));
const sent = await sendFolder(device, `${PROJECT}/data`);
equal(sent, 2, "every file of the project's data folder sent, subfolders too");
equal(decoder.decode(await readFile(device, "kept.txt")), "mine", "the other files of the device are kept");

const projectBuild = build(PROJECT);
await upload(device, fs.readFileSync(projectBuild.file));
await device.request(KIND.RUN);
check(await printed('config: { "speed": 3 }'), "the program reads the file sent (replaced)");
check(await printed("note: nested"), "and the one of a subfolder");

device.close();
fs.rmSync(`${PROJECT}/build`, { recursive: true, force: true });
report("datafiles");
