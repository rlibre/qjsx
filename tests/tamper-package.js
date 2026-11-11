/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Test helper (tests/run-tests.sh): copies a package or a library as if
// another engine had compiled it. The copy says another bytecode version in
// its manifest; a package also gets another build id, so that it can be told
// from the original once installed.
// Usage: qjsx run tests/tamper-package.js <package or library> <copy>
import * as fs from "fs";
import { readZip, writeZip } from "qjsx:zip";

const [original, copy] = process.argv.slice(2);
const entries = readZip(fs.readFileSync(original));

const manifest = JSON.parse(new TextDecoder().decode(entries.get("manifest.json")));
manifest.bytecode += 1;
if (manifest.buildId !== undefined) {
    manifest.buildId = "0000" + manifest.buildId.slice(4);
}
entries.set("manifest.json", new TextEncoder().encode(JSON.stringify(manifest, null, 1)));

const copied = [...entries].map(([name, data]) => ({ name, data }));
fs.writeFileSync(copy, writeZip(copied));
