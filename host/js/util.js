/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Small helpers shared by the tool modules.

/**
 * Lowercase hexadecimal text of 'bytes', two digits per byte.
 */
export function hex(bytes) {
    let text = "";

    for (const byte of bytes) {
        text += byte.toString(16).padStart(2, "0");
    }

    return text;
}

const encoder = new TextEncoder();
const decoder = new TextDecoder();

/**
 * UTF-8 bytes of 'text', as a Uint8Array.
 */
export function encodeText(text) {
    return encoder.encode(text);
}

/**
 * "file:line:column" of a byte offset of a source given as its UTF-8 bytes
 * (line and column from 1, the column in characters): where an error of the
 * build points.
 */
export function placeOf(path, sourceBytes, offset) {
    const before = decoder.decode(sourceBytes.subarray(0, offset));
    const lineStart = before.lastIndexOf("\n") + 1;
    const line = before.split("\n").length;
    const column = before.length - lineStart + 1;
    return `${path}:${line}:${column}`;
}
