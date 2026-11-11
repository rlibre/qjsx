/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Zip archives with stored (uncompressed) entries, the format of .qjspkg
// packages: the ESP can then use the entries in place, without copying.
// Names are UTF-8; dates are fixed (1980-01-01) so that the same content
// gives the same archive.

const SIGNATURE_LOCAL = 0x04034b50;
const SIGNATURE_CENTRAL = 0x02014b50;
const SIGNATURE_END = 0x06054b50;
const VERSION = 20;              // 2.0: enough for stored entries
const FLAG_UTF8_NAMES = 0x0800;
const METHOD_STORED = 0;
const DOS_TIME = 0;
const DOS_DATE = (0 << 9) | (1 << 5) | 1;   // 1980-01-01

const crcTable = (() => {
    const table = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++) {
            c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
        }
        table[n] = c >>> 0;
    }
    return table;
})();

export function crc32(bytes) {
    let crc = 0xffffffff;
    for (let i = 0; i < bytes.length; i++) {
        crc = crcTable[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
    }
    return (crc ^ 0xffffffff) >>> 0;
}

function concat(parts) {
    const size = parts.reduce((total, part) => total + part.length, 0);
    const result = new Uint8Array(size);
    let offset = 0;
    for (const part of parts) {
        result.set(part, offset);
        offset += part.length;
    }
    return result;
}

// entries: [{ name: string, data: Uint8Array }] -> Uint8Array
export function writeZip(entries) {
    const encoder = new TextEncoder();
    const localParts = [];
    const centralParts = [];
    let offset = 0;
    for (const entry of entries) {
        const name = encoder.encode(entry.name);
        const data = entry.data;
        const crc = crc32(data);

        const local = new Uint8Array(30 + name.length);
        const localView = new DataView(local.buffer);
        localView.setUint32(0, SIGNATURE_LOCAL, true);
        localView.setUint16(4, VERSION, true);
        localView.setUint16(6, FLAG_UTF8_NAMES, true);
        localView.setUint16(8, METHOD_STORED, true);
        localView.setUint16(10, DOS_TIME, true);
        localView.setUint16(12, DOS_DATE, true);
        localView.setUint32(14, crc, true);
        localView.setUint32(18, data.length, true);   // compressed size
        localView.setUint32(22, data.length, true);   // uncompressed size
        localView.setUint16(26, name.length, true);
        localView.setUint16(28, 0, true);             // extra field length
        local.set(name, 30);

        const central = new Uint8Array(46 + name.length);
        const centralView = new DataView(central.buffer);
        centralView.setUint32(0, SIGNATURE_CENTRAL, true);
        centralView.setUint16(4, VERSION, true);      // made by
        centralView.setUint16(6, VERSION, true);      // needed to extract
        centralView.setUint16(8, FLAG_UTF8_NAMES, true);
        centralView.setUint16(10, METHOD_STORED, true);
        centralView.setUint16(12, DOS_TIME, true);
        centralView.setUint16(14, DOS_DATE, true);
        centralView.setUint32(16, crc, true);
        centralView.setUint32(20, data.length, true);
        centralView.setUint32(24, data.length, true);
        centralView.setUint16(28, name.length, true);
        centralView.setUint32(42, offset, true);      // offset of the local header
        central.set(name, 46);

        localParts.push(local, data);
        centralParts.push(central);
        offset += local.length + data.length;
    }
    const centralDirectory = concat(centralParts);
    const end = new Uint8Array(22);
    const endView = new DataView(end.buffer);
    endView.setUint32(0, SIGNATURE_END, true);
    endView.setUint16(8, entries.length, true);       // entries on this disk
    endView.setUint16(10, entries.length, true);      // entries in total
    endView.setUint32(12, centralDirectory.length, true);
    endView.setUint32(16, offset, true);              // offset of the central directory
    return concat([...localParts, centralDirectory, end]);
}

// Uint8Array -> Map(name -> Uint8Array view). Stored entries only.
export function readZip(bytes) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let end = bytes.length - 22;
    while (end >= 0 && view.getUint32(end, true) !== SIGNATURE_END) {
        end--;
    }
    if (end < 0) {
        throw new Error("not a zip archive");
    }
    const count = view.getUint16(end + 10, true);
    let position = view.getUint32(end + 16, true);
    const decoder = new TextDecoder();
    const entries = new Map();
    for (let i = 0; i < count; i++) {
        if (view.getUint32(position, true) !== SIGNATURE_CENTRAL) {
            throw new Error("invalid central directory");
        }
        const method = view.getUint16(position + 10, true);
        const size = view.getUint32(position + 20, true);
        const nameLength = view.getUint16(position + 28, true);
        const extraLength = view.getUint16(position + 30, true);
        const commentLength = view.getUint16(position + 32, true);
        const localOffset = view.getUint32(position + 42, true);
        const name = decoder.decode(bytes.subarray(position + 46, position + 46 + nameLength));
        if (method !== METHOD_STORED) {
            throw new Error(`${name}: compressed entries are not supported`);
        }
        const dataOffset = localOffset + 30 + view.getUint16(localOffset + 26, true) + view.getUint16(localOffset + 28, true);
        entries.set(name, bytes.subarray(dataOffset, dataOffset + size));
        position += 46 + nameLength + extraLength + commentLength;
    }
    return entries;
}
