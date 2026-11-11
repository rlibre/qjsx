/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tests of the protocol modules: CBOR, COBS, CRC-16, frames, messages.
// Run with: bin/Release/qjsx run tests/protocol.test.js
import { encode, decode } from "qjsx:cbor";
import { crc16, cobsEncode, cobsDecode, encodeFrame, FrameParser } from "qjsx:frames";
import { KIND, KEY, encodeMessage, decodeMessage } from "qjsx:protocol";
import { hex } from "qjsx:util";
import { check, equal, throws, report } from "./assert.js";

const bytes = (...values) => new Uint8Array(values);
const text = string => new TextEncoder().encode(string);

// --- CBOR: encodings from RFC 8949, appendix A
const vectors = [
    [0, "00"], [23, "17"], [24, "1818"], [100, "1864"], [1000, "1903e8"], [1000000, "1a000f4240"],
    [1000000000000, "1b000000e8d4a51000"], [-1, "20"], [-1000, "3903e7"], [1.5, "fb3ff8000000000000"],
    [false, "f4"], [true, "f5"], [null, "f6"], [undefined, "f7"], ["", "60"], ["IETF", "6449455446"],
    ["ü", "62c3bc"], [[1, 2, 3], "83010203"], [bytes(1, 2, 3, 4), "4401020304"],
];
for (const [value, expected] of vectors) {
    equal(hex(encode(value)), expected, `cbor encode ${String(value)}`);
}
equal(decode(Uint8Array.from([0xf9, 0x3c, 0x00])), 1, "half float");
equal(decode(Uint8Array.from([0xfa, 0x47, 0xc3, 0x50, 0x00])), 100000, "single float");
const nested = decode(encode({ 1: "a", 2: [true, null, bytes(9)], name: { 3: -5 } }));
check(nested instanceof Map && nested.get(1) === "a", "map with integer keys");
equal(nested.get(2)[2][0], 9, "bytes in an array in a map");
equal(nested.get("name").get(3), -5, "string key and nested map");
throws(() => decode(bytes(0x83, 0x01)), /truncated/, "truncated data");
throws(() => decode(bytes(0x01, 0x02)), /extra bytes/, "extra bytes");

// --- CRC and COBS
equal(crc16(text("123456789")), 0x29b1, "CRC-16/CCITT-FALSE check value");
const cobsVectors = [
    [[0x00], "0101"], [[0x00, 0x00], "010101"], [[0x11, 0x22, 0x00, 0x33], "03112202 33".replace(" ", "")],
    [[0x11, 0x22, 0x33, 0x44], "0511223344"], [[0x11, 0x00, 0x00, 0x00], "0211010101"],
];
for (const [input, expected] of cobsVectors) {
    const encoded = cobsEncode(Uint8Array.from(input));
    equal(hex(encoded), expected, `cobs ${input}`);
    equal(hex(cobsDecode(encoded)), hex(Uint8Array.from(input)), `cobs round trip ${input}`);
}
const long = new Uint8Array(600).map((_, i) => (i % 255) + 1);   // no zero: 254-byte blocks
equal(hex(cobsDecode(cobsEncode(long))), hex(long), "cobs long run");
check(!cobsEncode(long).includes(0), "no zero after cobs");

// --- frames mixed with console text, split in arbitrary chunks
const messages = [];
const texts = [];
const parser = new FrameParser(message => messages.push(decodeMessage(message)), segment => texts.push(new TextDecoder().decode(segment)));
const frame1 = encodeFrame(encodeMessage(KIND.HELLO, 1, { [KEY.PROTOCOL]: 1 }));
const frame2 = encodeFrame(encodeMessage(KIND.LOG, 0, { [KEY.STREAM]: 1, [KEY.TEXT]: "hi\n" }));
const stream = Uint8Array.from([...text("boot rom v1\n"), ...frame1, ...text("noise"), ...frame2]);
for (let i = 0; i < stream.length; i += 5) {
    parser.feed(stream.subarray(i, i + 5));
}
equal(messages.length, 2, "two frames found");
equal(messages[0].kind, KIND.HELLO, "first message kind");
equal(messages[0].id, 1, "first message id");
equal(messages[0].payload.get(KEY.PROTOCOL), 1, "first message payload");
equal(messages[1].payload.get(KEY.TEXT), "hi\n", "second message payload");
equal(texts.join("|"), "boot rom v1\n|noise", "console text kept apart");

// --- a corrupted frame is ignored (CRC)
const corrupted = encodeFrame(encodeMessage(KIND.INFO, 2, {}));
corrupted[corrupted.length - 2] ^= 0x40;
messages.length = 0;
parser.feed(corrupted);
equal(messages.length, 0, "corrupted frame rejected");

report("protocol");
