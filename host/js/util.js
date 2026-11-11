/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Small helpers shared by the tool modules.

export function hex(bytes) {
    let text = "";
    for (const byte of bytes) {
        text += byte.toString(16).padStart(2, "0");
    }
    return text;
}

const encoder = new TextEncoder();

export function encodeText(text) {
    return encoder.encode(text);
}
