/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// "qjsx build --executable": a program made into an executable of the PC. A
// copy of qjsx-runtime (built next to this tool, host/runtime/) with the
// package appended, then a trailer that qjsx-runtime looks for at the end of
// its own file:
//
//   [qjsx-runtime] [package] ["QJSXPKG1"] [size of the package: 64 bits, little endian]
//
// The executable runs the program like "qjsx run package.qjspkg" (from its
// bytecode, without compiler), with the native modules of the PC.
import * as fs from "sys/fs";
import { dirname, join } from "stdlib/path";

const TRAILER_MAGIC = "QJSXPKG1";   // 8 ASCII bytes
const TRAILER_SIZE = 16;            // magic + 64-bit size

// Path of qjsx-runtime: next to this tool (process.argv[0] is its full path).
export function runtimePath() {
    const name = process.platform === "win32" ? "qjsx-runtime.exe" : "qjsx-runtime";
    return join(dirname(process.argv[0]), name);
}

// The trailer that follows a package of 'size' bytes.
function trailer(size) {
    const bytes = new Uint8Array(TRAILER_SIZE);
    for (let i = 0; i < TRAILER_MAGIC.length; i++) {
        bytes[i] = TRAILER_MAGIC.charCodeAt(i);
    }

    // the size, 64 bits little endian (a package is far below 4 GB)
    const view = new DataView(bytes.buffer);
    view.setUint32(8, size, true);
    view.setUint32(12, 0, true);
    return bytes;
}

// Writes 'outputFile': qjsx-runtime followed by the package 'packageFile'
// and the trailer. Returns { file, size }. On Linux and macOS the file is not
// marked executable (no chmod in sys/fs yet): 'chmod +x' it.
export function makeExecutable(packageFile, outputFile) {
    // the runner and the package
    const runtimeFile = runtimePath();
    if (!fs.existsSync(runtimeFile)) {
        throw new Error(`${runtimeFile} not found: qjsx-runtime is built with qjsx (premake project qjsx-runtime)`);
    }

    const runtime = fs.readFileSync(runtimeFile);
    const packageBytes = fs.readFileSync(packageFile);

    // one block: runner, package, trailer
    const executable = new Uint8Array(runtime.length + packageBytes.length + TRAILER_SIZE);
    executable.set(runtime, 0);
    executable.set(packageBytes, runtime.length);
    executable.set(trailer(packageBytes.length), runtime.length + packageBytes.length);

    fs.writeFileSync(outputFile, executable);
    return { file: outputFile, size: executable.length };
}
