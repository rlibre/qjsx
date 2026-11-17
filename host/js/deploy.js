/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Sends a package to a device and starts it.
//
// The device gets a reduced package: manifest.json (with "reduced": true),
// names.bin and bytecode/ only, what its loader reads. The debug sidecars (debug/) and
// the sources (src/) stay in the full package on the PC, where the debugger
// finds it by its build id, which does not change.
import * as fs from "sys/fs";
import { sha256 } from "sys/crypto";
import { readZip, writeZip } from "qjsx:zip";
import { KIND, KEY, UPLOAD_CHUNK } from "qjsx:protocol";

// The entry of the names of a package and its alignment (as qjsx:build).
const NAMES_ENTRY = "names.bin";
const NAMES_ALIGNMENT = 8;

/**
 * The manifest of a package (its manifest.json, parsed).
 */
export function packageManifest(packageBytes) {
    const manifest = readZip(packageBytes).get("manifest.json");
    if (manifest === undefined) {
        throw new Error("not a package: no manifest.json");
    }

    return JSON.parse(new TextDecoder().decode(manifest));
}

/**
 * The most recent package of a project output directory, or undefined.
 */
export function latestPackage(directory, name) {
    if (!fs.existsSync(directory)) {
        return undefined;
    }

    // packages are named <name>-<12 hex digits of the build id>.qjspkg
    const escapedName = name.replace(/[.]/g, "\\.");
    const pattern = new RegExp(`^${escapedName}-[0-9a-f]{12}\\.qjspkg$`);

    // the one modified last
    const newest = fs.readdirSync(directory)
        .filter(file => pattern.test(file))
        .map(file => ({ path: `${directory}/${file}`, time: fs.statSync(`${directory}/${file}`).mtimeMs }))
        .sort((a, b) => b.time - a.time)[0];
    return newest?.path;
}

/**
 * The reduced package of a full one: its manifest marked "reduced": true and
 * its bytecode/ entries, stored as in any package (read in place). The
 * scripts of the manifest lose their 'debug' and 'source' entries, which are
 * not in the reduced package.
 */
export function reducePackage(packageBytes) {
    // the manifest without what the device does not get
    const entries = readZip(packageBytes);
    const manifest = packageManifest(packageBytes);
    const scripts = manifest.scripts.map(({ debug, source, ...kept }) => kept);
    const reducedManifest = { ...manifest, scripts, reduced: true };
    const manifestText = JSON.stringify(reducedManifest, null, 1);

    // then the entries the loader reads
    const kept = [{ name: "manifest.json", data: new TextEncoder().encode(manifestText) }];

    for (const [name, data] of entries) {
        if (name.startsWith("bytecode/")) {
            kept.push({ name, data });
        }

        // the names the bytecode is linked to, aligned as in the full
        // package (the engine uses them where the package lies)
        if (name === NAMES_ENTRY) {
            kept.push({ name, data, align: NAMES_ALIGNMENT });
        }
    }

    return writeZip(kept);
}

/**
 * The native modules a program needs that the device does not have, or []
 * when the device does not say which ones it has (an older firmware).
 */
export function missingNatives(device, manifest) {
    const deviceListsNatives = Array.isArray(device.natives);
    if (!deviceListsNatives) {
        return [];
    }

    return (manifest.natives ?? []).filter(name => !device.natives.includes(name));
}

/**
 * Uploads the package 'packageBytes' (Uint8Array, a full package) to an open
 * device (session started), reduced (see reducePackage). Options:
 *   - onProgress(sentBytes, totalBytes);
 *   - ram: true loads it in the RAM of the device instead of installing it in
 *     flash: nothing is written to the flash, the package is lost at the next
 *     restart of the device, RUN starts it while it is there.
 * Refused before anything is written when:
 *   - the program needs native modules the device does not have (its answer
 *     to HELLO lists them);
 *   - the device reads another bytecode version (it refuses UPLOAD_BEGIN);
 *   - in RAM: the device has not enough free RAM for it.
 * Returns the build id of the package.
 */
export async function upload(device, packageBytes, { onProgress, ram = false } = {}) {
    // the package must say which bytecode version it holds
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

    // announce the reduced package: the device checks it can take it
    const reduced = reducePackage(packageBytes);
    const buildId = manifest.buildId;
    await device.request(KIND.UPLOAD_BEGIN, {
        [KEY.SIZE]: reduced.length,
        [KEY.SHA256]: sha256(reduced),
        [KEY.BUILD_ID]: buildId,
        [KEY.BYTECODE]: manifest.bytecode,
        [KEY.VOLATILE]: ram,
    });

    // send it chunk by chunk
    for (let offset = 0; offset < reduced.length; offset += UPLOAD_CHUNK) {
        const chunk = reduced.subarray(offset, offset + UPLOAD_CHUNK);
        await device.request(KIND.UPLOAD_CHUNK, { [KEY.OFFSET]: offset, [KEY.DATA]: chunk });
        onProgress?.(offset + chunk.length, reduced.length);
    }

    // the device checks the whole package and installs it
    await device.request(KIND.UPLOAD_END, {});
    return buildId;
}
