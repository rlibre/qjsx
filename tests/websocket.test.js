/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tests of crypto.sha1, of the standard base64 of the engine, and of
// qjsx:websocket: frames, handshake, a server and a client talking.
// Run with: bin/Release/qjsx run tests/websocket.test.js
import { connect as netConnect } from "net";
import { sha1 } from "crypto";
import { hex } from "qjsx:util";
import { acceptKey, encodeFrame, FrameParser, OPCODE, startServer, connect } from "qjsx:websocket";
import { check, equal, report } from "./assert.js";

const encoder = new TextEncoder();

// --- SHA-1 (FIPS 180 test vectors)
equal(hex(sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1 of nothing");
equal(hex(sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1 of 'abc'");
equal(hex(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")), "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
    "sha1 over two blocks");
equal(hex(sha1(encoder.encode("The quick brown fox jumps over the lazy dog"))), "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12",
    "sha1 of bytes");

// --- base64 of the engine (Uint8Array.prototype.toBase64, Uint8Array.fromBase64)
equal(encoder.encode("hello").toBase64(), "aGVsbG8=", "toBase64");
equal(new TextDecoder().decode(Uint8Array.fromBase64("aGVsbG8=")), "hello", "fromBase64");

// --- handshake key (example of RFC 6455, 1.3)
equal(acceptKey("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "Sec-WebSocket-Accept");

// --- frames: every length encoding, masked or not, fed in small pieces
for (const size of [0, 5, 125, 126, 300, 70000]) {
    const payload = new Uint8Array(size).map((_, index) => index & 0xff);
    for (const mask of [undefined, new Uint8Array([1, 2, 3, 4])]) {
        const frames = [];
        const parser = new FrameParser(frame => frames.push(frame));
        const encoded = encodeFrame(OPCODE.BINARY, payload, mask);
        for (let offset = 0; offset < encoded.length; offset += 1000) {
            parser.feed(encoded.subarray(offset, offset + 1000));
        }
        const same = frames.length === 1 && frames[0].payload.length === size && frames[0].payload.every((byte, index) => byte === (index & 0xff));
        check(same && frames[0].fin && frames[0].opcode === OPCODE.BINARY, `frame of ${size} bytes${mask ? ", masked" : ""}`);
    }
}

// --- a server and a client: JSON routes, upgrade, echo
const server = startServer({
    port: 0,
    routes: path => (path === "/json/version" ? { Browser: "test" } : undefined),
    acceptsSocket: path => path === "/echo",
    onSocket: socket => socket.onMessage(text => socket.send(`echo:${text}`)),
});

// a plain HTTP GET, answered with JSON
async function httpGet(path) {
    const connection = await netConnect({ port: server.port() });
    return new Promise(resolve => {
        let text = "";
        connection.onData(bytes => {
            text += new TextDecoder().decode(bytes);
        });
        connection.onClose(() => resolve(text));
        connection.write(`GET ${path} HTTP/1.1\r\nHost: localhost\r\n\r\n`);
    });
}

const version = await httpGet("/json/version");
check(version.startsWith("HTTP/1.1 200") && version.includes("\"Browser\": \"test\""), "GET /json/version answered with JSON");
const missing = await httpGet("/nothing");
check(missing.startsWith("HTTP/1.1 404"), "GET of an unknown path: 404");

let refused = false;
try {
    await connect({ port: server.port(), path: "/other" });
} catch (error) {
    refused = true;
}
check(refused, "a WebSocket on a path not accepted is refused");

const socket = await connect({ port: server.port(), path: "/echo" });
const answers = [];
let answered;
socket.onMessage(text => {
    answers.push(text);
    if (answers.length === 2) {
        answered();
    }
});
const bothAnswered = new Promise(resolve => {
    answered = resolve;
});
const long = "x".repeat(100000);
socket.send("hello é");
socket.send(long);
await bothAnswered;
equal(answers[0], "echo:hello é", "a text message and its echo (UTF-8)");
equal(answers[1], `echo:${long}`, "a long message (64-bit length)");

socket.close();
server.close();
report("websocket");
