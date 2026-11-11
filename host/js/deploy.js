// Sends a package to a device and starts it.
import * as fs from "fs";
import { sha256 } from "crypto";
import { readZip } from "espjs:zip";
import { KIND, KEY, UPLOAD_CHUNK } from "espjs:protocol";

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

// Uploads 'packageBytes' (Uint8Array) to an open device (session started).
// onProgress(sentBytes, totalBytes) is optional. The device is told which
// bytecode version the package holds: it refuses the upload at once, before
// writing anything, if its engine reads another one.
export async function upload(device, packageBytes, onProgress) {
    const manifest = packageManifest(packageBytes);
    if (typeof manifest.bytecode !== "number") {
        throw new Error("the package does not say which bytecode version it holds: rebuild it");
    }
    const buildId = manifest.buildId;
    await device.request(KIND.UPLOAD_BEGIN, {
        [KEY.SIZE]: packageBytes.length,
        [KEY.SHA256]: sha256(packageBytes),
        [KEY.BUILD_ID]: buildId,
        [KEY.BYTECODE]: manifest.bytecode,
    });
    for (let offset = 0; offset < packageBytes.length; offset += UPLOAD_CHUNK) {
        const chunk = packageBytes.subarray(offset, offset + UPLOAD_CHUNK);
        await device.request(KIND.UPLOAD_CHUNK, { [KEY.OFFSET]: offset, [KEY.DATA]: chunk });
        onProgress?.(offset + chunk.length, packageBytes.length);
    }
    await device.request(KIND.UPLOAD_END, {});
    return buildId;
}
