/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * Hash functions: SHA-256, and SHA-1 for the protocols that impose it.
 *
 * Not Node's `crypto`: plain functions that return the digest as bytes.
 * Base64 is not here: the engine has the standard
 * `Uint8Array.prototype.toBase64()`, `toHex()` and `Uint8Array.fromBase64()`.
 *
 * @example
 * import { sha256 } from "sys/crypto";
 * const hex = sha256("hello").toHex();
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | ✓ | ✓ | ✓ | ✓ |
 *
 * @module sys/crypto
 */
declare module "sys/crypto" {
    /**
     * Data to hash: a string (hashed as its UTF-8 bytes), any typed array (its
     * own bytes) or an ArrayBuffer. A DataView or an array of numbers is
     * refused.
     */
    export type HashData = string | ArrayBuffer | Int8Array | Uint8Array | Uint8ClampedArray | Int16Array | Uint16Array | Int32Array | Uint32Array | Float16Array | Float32Array | Float64Array | BigInt64Array | BigUint64Array;

    /**
     * The SHA-256 digest of `data`, in a new 32-byte Uint8Array.
     *
     * @throws TypeError if `data` is neither a string nor bytes.
     */
    export function sha256(data: HashData): Uint8Array;

    /**
     * The SHA-1 digest of `data`, in a new 20-byte Uint8Array. SHA-1 is
     * broken: use it only where a protocol requires it (the WebSocket
     * handshake...), else {@link sha256}.
     *
     * @throws TypeError if `data` is neither a string nor bytes.
     */
    export function sha1(data: HashData): Uint8Array;
}
