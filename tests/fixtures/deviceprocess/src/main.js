/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What process looks like to a program: on a device (the desktop agent) argv
// is ["qjsx", main module], env comes from qjsx.json, cwd is the data
// directory of the board and fs resolves relative paths against it. Run on
// the PC (qjsx run), argv and env are the real ones.
import * as fs from "fs";

console.log(`argv ${JSON.stringify(process.argv)}`);
console.log(`env ${JSON.stringify(process.env)}`);

// only on a device: the file goes into the data directory
if (process.cwd().endsWith("/data")) {
    fs.writeFileSync("note.txt", "written by the program");
    console.log(`read back: ${fs.readFileSync("note.txt", "utf8")}`);
    console.log(`found from cwd: ${fs.existsSync(process.cwd() + "/note.txt")}`);
}
