/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Sends a package to a device and starts it.
//
// The device gets a reduced package: manifest.json (with "reduced": true)
// and bytecode/ only, what its loader reads. The debug sidecars (debug/) and
// the sources (src/) stay in the full package on the PC, where the debugger
// finds it by its build id, which does not change.
import * as fs from "sys/fs";
import { sha256 } from "sys/crypto";
import { readZip, writeZip } from "qjsx:zip";
import { KIND, KEY, UPLOAD_CHUNK } from "qjsx:protocol";

// The manifest of a package (its manifest.json, parsed).
export function packageManifest(packageBytes) {
    const manifest = readZip(packageBytes).get("manifest.json");
    if (manifest === undefined) {
        throw new Error("not a package: no manifest.json");
    }
    return JSON.parse(new TextDecoder().decode(manifest));
}

// The most recent package of a project output directory, or undefined.
export function latestPackage(directory, name) {
    if (!fs.existsSync(directory)) {
        return undefined;
    }
    const pattern = new RegExp(`^${name.replace(/[.]/g, "\\.")}-[0-9a-f]{12}\\.qjspkg$`);
    const newest = fs.readdirSync(directory)
        .filter(file => pattern.test(file))
        .map(file => ({ path: `${directory}/${file}`, time: fs.statSync(`${directory}/${file}`).mtimeMs }))
        .sort((a, b) => b.time - a.time)[0];
    return newest?.path;
}

// The reduced package of a full one: its manifest marked "reduced": true and
// its bytecode/ entries, stored as in any package (read in place). The
// scripts of the manifest lose their 'debug' and 'source' entries, which are
// not in the reduced package.
export function reducePackage(packageBytes) {
    const entries = readZip(packageBytes);
    const manifest = packageManifest(packageBytes);
    const scripts = manifest.scripts.map(({ debug, source, ...kept }) => kept);
    const reducedManifest = { ...manifest, scripts, reduced: true };

    const kept = [{ name: "manifest.json", data: new TextEncoder().encode(JSON.stringify(reducedManifest, null, 1)) }];
    for (const [name, data] of entries) {
        if (name.startsWith("bytecode/")) {
            kept.push({ name, data });
        }
    }
    return writeZip(kept);
}

// The native modules a program needs that the device does not have, or []
// when the device does not say which ones it has (an older firmware).
export function missingNatives(device, manifest) {
    if (!Array.isArray(device.natives)) {
        return [];
    }
    return (manifest.natives ?? []).filter(name => !device.natives.includes(name));
}

// Uploads the package 'packageBytes' (Uint8Array, a full package) to an open
// device (session started), reduced (see reducePackage). Options:
//   - onProgress(sentBytes, totalBytes);
//   - ram: true loads it in the RAM of the device instead of installing it in
//     flash: nothing is written to the flash, the package is lost at the next
//     restart of the device, RUN starts it while it is there.
// Refused before anything is written when:
//   - the program needs native modules the device does not have (its answer
//     to HELLO lists them);
//   - the device reads another bytecode version (it refuses UPLOAD_BEGIN);
//   - in RAM: the device has not enough free RAM for it.
export async function upload(device, packageBytes, { onProgress, ram = false } = {}) {
    const manifest = packageManifest(packageBytes);
    if (typeof manifest.bytecode !== "number") {
        throw new Error("the package does not say which bytecode version it holds: rebuild it");
    }

    // every native module the program imports must be in the firmware
    const missing = missingNatives(device, manifest);
    if (missing.length > 0) {
        throw new Error(`the program needs native module(s) ${missing.join(", ")}, which this device does not have`
            + ` (it has: ${device.natives.join(", ")})`);
    }

    // send the reduced package
    const reduced = reducePackage(packageBytes);
    const buildId = manifest.buildId;
    await device.request(KIND.UPLOAD_BEGIN, {
        [KEY.SIZE]: reduced.length,
        [KEY.SHA256]: sha256(reduced),
        [KEY.BUILD_ID]: buildId,
        [KEY.BYTECODE]: manifest.bytecode,
        [KEY.VOLATILE]: ram,
    });
    for (let offset = 0; offset < reduced.length; offset += UPLOAD_CHUNK) {
        const chunk = reduced.subarray(offset, offset + UPLOAD_CHUNK);
        await device.request(KIND.UPLOAD_CHUNK, { [KEY.OFFSET]: offset, [KEY.DATA]: chunk });
        onProgress?.(offset + chunk.length, reduced.length);
    }
    await device.request(KIND.UPLOAD_END, {});
    return buildId;
}
