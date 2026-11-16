/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The files of the data folder of a device (its "/data"; the desktop agent
// and the twin: <storage>/data), through the FILE_* requests of the agent
// (qjsx:protocol). Paths are relative to that folder, '/' separators. A
// refused request throws the DeviceError of the agent, with its Node-like
// `code` ("ENOENT", "ENOSYS": the device has no file system...).
//
// Used by "qjsx device ls | read | write | rm | mkdir | mv" and by
// "qjsx deploy --data" (the "data" folder of the project).
import * as fs from "sys/fs";
import { join } from "stdlib/path";
import { KIND, KEY, UPLOAD_CHUNK } from "qjsx:protocol";

// The entries of folder 'path' ("" or "/": the data folder): [{ name, size,
// isDirectory }], sorted by name.
export async function listFiles(device, path) {
    const answer = await device.request(KIND.FILE_LIST, { [KEY.PATH]: path });
    const entries = answer.get(KEY.ENTRIES).map(([name, size, isDirectory]) => ({ name, size, isDirectory }));
    return entries.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
}

// The whole content of file 'path', a Uint8Array, read piece by piece.
export async function readFile(device, path) {
    const pieces = [];
    let offset = 0;
    let size = 0;

    // a piece at a time, until the size the device reports
    do {
        const answer = await device.request(KIND.FILE_READ, { [KEY.PATH]: path, [KEY.OFFSET]: offset });
        const data = answer.get(KEY.DATA);
        size = answer.get(KEY.SIZE);
        pieces.push(data);
        offset += data.length;
        if (data.length === 0) {
            break;   // the file got shorter meanwhile
        }
    } while (offset < size);

    // in one array
    const content = new Uint8Array(offset);
    let position = 0;
    for (const piece of pieces) {
        content.set(piece, position);
        position += piece.length;
    }
    return content;
}

// Writes 'bytes' (a Uint8Array) to file 'path': the device keeps the old
// content until the last piece arrived, then puts the new file in place.
export async function writeFile(device, path, bytes) {
    await device.request(KIND.FILE_WRITE_BEGIN, { [KEY.PATH]: path, [KEY.SIZE]: bytes.length });
    for (let offset = 0; offset < bytes.length; offset += UPLOAD_CHUNK) {
        const data = bytes.subarray(offset, offset + UPLOAD_CHUNK);
        await device.request(KIND.FILE_WRITE_CHUNK, { [KEY.OFFSET]: offset, [KEY.DATA]: data });
    }
    await device.request(KIND.FILE_WRITE_END);
}

// Removes file 'path', or folder 'path' if empty.
export async function removeFile(device, path) {
    await device.request(KIND.FILE_REMOVE, { [KEY.PATH]: path });
}

// Creates folder 'path' (its parent must exist).
export async function makeDirectory(device, path) {
    await device.request(KIND.FILE_MKDIR, { [KEY.PATH]: path });
}

// Renames 'from' to 'to' (a file replaces a file; folders never replace).
export async function renameFile(device, from, to) {
    await device.request(KIND.FILE_RENAME, { [KEY.PATH]: from, [KEY.TO]: to });
}

// Sends every file of the local folder 'folder' (its subfolders too) to the
// data folder of the device, at the same relative paths: a file of the same
// name is replaced, the other files of the device stay (what the program
// wrote itself). 'onFile(path, size)' is told each file. Returns the number
// of files sent.
export async function sendFolder(device, folder, onFile = () => {}) {
    let count = 0;

    // one folder: its subfolders created (they may exist), its files written
    async function sendLevel(local, remote) {
        for (const name of fs.readdirSync(local).sort()) {
            const localPath = join(local, name);
            const remotePath = remote === "" ? name : `${remote}/${name}`;
            if (fs.statSync(localPath).isDirectory()) {
                try {
                    await makeDirectory(device, remotePath);
                } catch (error) {
                    if (error.code !== "EEXIST") {
                        throw error;
                    }
                }
                await sendLevel(localPath, remotePath);
                continue;
            }

            const bytes = fs.readFileSync(localPath);
            await writeFile(device, remotePath, bytes);
            onFile(remotePath, bytes.length);
            count++;
        }
    }

    await sendLevel(folder, "");
    return count;
}
