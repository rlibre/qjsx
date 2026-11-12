/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tries what a program of the PC may reach (tests/run-tests.sh, "permissions"):
// files inside its root folder, outside it, through "..", through a link,
// the network, the environment. Prints "<probe>: ok" or "<probe>: <error
// code>". The last argument is a folder outside the root (it holds
// existing.txt).
//
// Safety: the destructive probe (removing the parent folder) only runs when
// the current folder is the test's own "perm-root", so that a broken check
// can only remove the test's folder.
import * as fs from "sys/fs";
import { listen } from "sys/net";

const outside = process.argv[process.argv.length - 1];

// Runs one probe and prints its result.
function probe(label, action) {
    try {
        action();
        console.log(`${label}: ok`);
    } catch (error) {
        console.log(`${label}: ${error.code ?? error.message}`);
    }
}

console.log(`cwd ${process.cwd()}`);
console.log(`env ${JSON.stringify(Object.keys(process.env))}`);
console.log(`args ${JSON.stringify(process.argv.slice(2))}`);

probe("write inside", () => fs.writeFileSync("inside.txt", "inside"));
probe("read inside", () => fs.readFileSync("inside.txt", "utf8"));
probe("write outside", () => fs.writeFileSync(`${outside}/outside.txt`, "outside"));
probe("read outside", () => fs.readFileSync(`${outside}/existing.txt`, "utf8"));
probe("escape with ..", () => fs.writeFileSync("../escaped.txt", "escaped"));
probe("read through a link", () => fs.readFileSync("link/existing.txt", "utf8"));
if (process.cwd().endsWith("/perm-root")) {
    probe("remove the parent", () => fs.rmSync("..", { recursive: true }));
}
probe("listen", () => listen({ port: 0 }, () => {}).close());

// the message of a refusal says how to grant the access
try {
    fs.readFileSync(`${outside}/existing.txt`);
} catch (error) {
    console.log(`message ${error.message}`);
}
