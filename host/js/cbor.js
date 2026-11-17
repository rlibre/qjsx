/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// CBOR (RFC 8949) subset used by the device agent protocol:
//   integers (up to 2^53), floats (decoded from 16/32/64 bits, encoded on 64),
//   byte strings (Uint8Array), text strings, arrays, maps (keys: integers or
//   strings, decoded into Map), false, true, null, undefined.
// No tags, no indefinite lengths.

const encoder = new TextEncoder();
const decoder = new TextDecoder();

// ### Encoding ################################################################

/**
 * Growing byte buffer that CBOR values are written into.
 */
class Writer {
    /**
     * Starts with a small buffer, grown on demand.
     */
    constructor() {
        this.bytes = new Uint8Array(64);
        this.length = 0;
    }

    /**
     * Makes room for 'size' more bytes, doubling the buffer as needed.
     */
    reserve(size) {
        // enough room already: nothing to do
        if (this.length + size <= this.bytes.length) {
            return;
        }

        // double the capacity until the bytes fit
        let capacity = this.bytes.length * 2;

        while (capacity < this.length + size) {
            capacity *= 2;
        }

        // copy the bytes written so far into the larger buffer
        const grown = new Uint8Array(capacity);
        grown.set(this.bytes.subarray(0, this.length));
        this.bytes = grown;
    }

    /**
     * Appends one byte.
     */
    byte(value) {
        this.reserve(1);
        this.bytes[this.length++] = value;
    }

    /**
     * Appends an array of bytes (Uint8Array or array of numbers).
     */
    append(bytes) {
        this.reserve(bytes.length);
        this.bytes.set(bytes, this.length);
        this.length += bytes.length;
    }

    /**
     * Major type + argument, in the shortest form.
     */
    head(major, value) {
        const type = major << 5;

        // the argument fits in the initial byte, or follows it on 1, 2, 4 or 8 bytes
        if (value < 24) {
            this.byte(type | value);
        } else if (value < 0x100) {
            this.byte(type | 24);
            this.byte(value);
        } else if (value < 0x10000) {
            this.byte(type | 25);
            this.byte(value >> 8);
            this.byte(value & 0xff);
        } else if (value < 0x100000000) {
            this.byte(type | 26);
            this.append([value >>> 24, (value >>> 16) & 0xff, (value >>> 8) & 0xff, value & 0xff]);
        } else {
            // 64 bits: bit operators work on 32 bits, so split into two halves
            this.byte(type | 27);
            const high = Math.floor(value / 0x100000000);
            const low = value >>> 0;
            this.append([high >>> 24, (high >>> 16) & 0xff, (high >>> 8) & 0xff, high & 0xff,
                         low >>> 24, (low >>> 16) & 0xff, (low >>> 8) & 0xff, low & 0xff]);
        }
    }

    /**
     * Appends any supported JS value (see the list at the top of the file).
     */
    value(value) {
        // simple values, numbers, strings, bytes, arrays, maps, then plain objects
        if (value === false) {
            this.byte(0xf4);
        } else if (value === true) {
            this.byte(0xf5);
        } else if (value === null) {
            this.byte(0xf6);
        } else if (value === undefined) {
            this.byte(0xf7);
        } else if (typeof value === "number") {
            this.number(value);
        } else if (typeof value === "string") {
            const bytes = encoder.encode(value);
            this.head(3, bytes.length);
            this.append(bytes);
        } else if (value instanceof Uint8Array) {
            this.head(2, value.length);
            this.append(value);
        } else if (Array.isArray(value)) {
            this.head(4, value.length);
            value.forEach(item => this.value(item));
        } else if (value instanceof Map) {
            this.head(5, value.size);
            value.forEach((item, key) => {
                this.value(key);
                this.value(item);
            });
        } else if (typeof value === "object") {
            // Plain object: keys that look like integers become integers.
            const entries = Object.entries(value);
            this.head(5, entries.length);

            for (const [key, item] of entries) {
                const isIntegerKey = /^(0|[1-9][0-9]*)$/.test(key);
                this.value(isIntegerKey ? Number(key) : key);
                this.value(item);
            }
        } else {
            throw new TypeError(`cbor: cannot encode a ${typeof value}`);
        }
    }

    /**
     * Appends a number: an integer when it is a safe one, else a 64-bit float.
     */
    number(value) {
        // integers: major type 0 (positive) or 1 (negative, stored as -1 - value)
        if (Number.isSafeInteger(value)) {
            if (value >= 0) {
                this.head(0, value);
            } else {
                this.head(1, -1 - value);
            }

            return;
        }

        // anything else: a float on 64 bits, big endian
        this.byte(0xfb);
        const bytes = new Uint8Array(8);
        new DataView(bytes.buffer).setFloat64(0, value);
        this.append(bytes);
    }
}

/**
 * JS value -> CBOR bytes (a Uint8Array of the exact size).
 */
export function encode(value) {
    const writer = new Writer();
    writer.value(value);
    return writer.bytes.slice(0, writer.length);
}

// ### Decoding ################################################################

/**
 * Reads CBOR values from a byte array, from the start.
 */
class Reader {
    /**
     * Reads 'bytes' (a Uint8Array) from its first byte.
     */
    constructor(bytes) {
        this.bytes = bytes;
        this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        this.position = 0;
    }

    /**
     * Throws when fewer than 'count' bytes are left.
     */
    need(count) {
        if (this.position + count > this.bytes.length) {
            throw new Error("cbor: truncated data");
        }
    }

    /**
     * Reads the argument of a head whose additional information is 'info'.
     */
    argument(info) {
        // small values are the additional information itself
        if (info < 24) {
            return info;
        }

        // else 24..27 say how many bytes follow
        const sizes = { 24: 1, 25: 2, 26: 4, 27: 8 };
        const size = sizes[info];
        if (size === undefined) {
            throw new Error(`cbor: unsupported additional information ${info}`);
        }

        // big endian; multiplications, not shifts: values above 2^32 are allowed
        this.need(size);
        let value = 0;

        for (let i = 0; i < size; i++) {
            value = value * 256 + this.bytes[this.position + i];
        }

        this.position += size;
        return value;
    }

    /**
     * Reads the next value.
     */
    value() {
        // initial byte: major type in the 3 high bits, additional information in the others
        this.need(1);
        const initial = this.bytes[this.position++];
        const major = initial >> 5;
        const info = initial & 0x1f;

        // one case per major type
        switch (major) {
            case 0: {
                return this.argument(info);
            }
            case 1: {
                return -1 - this.argument(info);
            }
            case 2: {
                const length = this.argument(info);
                this.need(length);
                const bytes = this.bytes.slice(this.position, this.position + length);
                this.position += length;
                return bytes;
            }
            case 3: {
                const length = this.argument(info);
                this.need(length);
                const text = decoder.decode(this.bytes.subarray(this.position, this.position + length));
                this.position += length;
                return text;
            }
            case 4: {
                const length = this.argument(info);
                const items = [];

                for (let i = 0; i < length; i++) {
                    items.push(this.value());
                }

                return items;
            }
            case 5: {
                const length = this.argument(info);
                const map = new Map();

                for (let i = 0; i < length; i++) {
                    const key = this.value();
                    map.set(key, this.value());
                }

                return map;
            }
            case 7: {
                return this.simple(info);
            }
            default: {
                throw new Error(`cbor: unsupported major type ${major}`);
            }
        }
    }

    /**
     * Reads a value of major type 7: false, true, null, undefined or a float.
     */
    simple(info) {
        switch (info) {
            case 20: {
                return false;
            }
            case 21: {
                return true;
            }
            case 22: {
                return null;
            }
            case 23: {
                return undefined;
            }
            case 25: {
                this.need(2);
                const half = this.view.getUint16(this.position);
                this.position += 2;
                return halfToNumber(half);
            }
            case 26: {
                this.need(4);
                const value = this.view.getFloat32(this.position);
                this.position += 4;
                return value;
            }
            case 27: {
                this.need(8);
                const value = this.view.getFloat64(this.position);
                this.position += 8;
                return value;
            }
            default: {
                throw new Error(`cbor: unsupported simple value ${info}`);
            }
        }
    }
}

/**
 * IEEE 754 half precision float (16 bits) -> number.
 */
function halfToNumber(half) {
    // fields: sign (1 bit), exponent (5 bits), fraction (10 bits)
    const sign = half & 0x8000 ? -1 : 1;
    const exponent = (half >> 10) & 0x1f;
    const fraction = half & 0x3ff;

    // exponent 0: subnormal numbers (and zero)
    if (exponent === 0) {
        return sign * fraction * 2 ** -24;
    }

    // exponent 31: infinities and NaN
    if (exponent === 31) {
        return fraction === 0 ? sign * Infinity : NaN;
    }

    // normal numbers
    return sign * (1 + fraction / 1024) * 2 ** (exponent - 15);
}

/**
 * CBOR bytes -> JS value; throws when the bytes hold more than one value.
 */
export function decode(bytes) {
    const reader = new Reader(bytes);
    const value = reader.value();
    if (reader.position !== bytes.length) {
        throw new Error("cbor: extra bytes after the value");
    }

    return value;
}
