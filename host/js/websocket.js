/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A small HTTP + WebSocket server (RFC 6455) on top of the "net" module, for
// the CDP server of the debugger (qjsx:cdp): a debugger such as VS Code's
// js-debug asks for /json/version or /json/list over HTTP, then opens a
// WebSocket and speaks JSON messages on it.
//
//   const server = startServer({ host: "127.0.0.1", port: 9339,
//       routes: path => (path === "/json/version" ? { Browser: "qjsx" } : undefined),
//       acceptsSocket: path => path === "/qjsx",
//       onSocket: socket => {
//           socket.onMessage(text => socket.send(text));   // echo
//           socket.onClose(() => ...);
//       },
//   });
//   server.port(); server.close();
//
// Only what a debugger needs: GET requests answered with JSON, text messages
// (fragmented or not), ping / pong, close. No extensions, no TLS. connect()
// is the client side, for the tests.
import { listen, connect as netConnect } from "sys/net";
import { sha1 } from "sys/crypto";

// Added to the key of the client to make Sec-WebSocket-Accept (RFC 6455 1.3).
const HANDSHAKE_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Frame opcodes (RFC 6455 5.2).
export const OPCODE = { CONTINUATION: 0x0, TEXT: 0x1, BINARY: 0x2, CLOSE: 0x8, PING: 0x9, PONG: 0xa };

// Largest HTTP request head and largest message accepted (bytes).
const MAX_REQUEST_HEAD = 16 * 1024;
const MAX_MESSAGE = 64 * 1024 * 1024;

const encoder = new TextEncoder();
const decoder = new TextDecoder();

// ### Handshake ###############################################################

// Sec-WebSocket-Accept of a Sec-WebSocket-Key: base64(SHA-1(key + GUID)).
export function acceptKey(key) {
    return sha1(key + HANDSHAKE_GUID).toBase64();
}

// The head of an HTTP request (text up to the blank line):
// { method, path, headers: Map(lower-case name -> value) }, or null if malformed.
export function parseRequestHead(text) {
    const lines = text.split("\r\n");
    const requestLine = /^([A-Z]+) (\S+) HTTP\/1\.[01]$/.exec(lines[0]);
    if (requestLine === null) {
        return null;
    }

    const headers = new Map();
    for (const line of lines.slice(1)) {
        const colon = line.indexOf(":");
        if (colon <= 0) {
            continue;
        }
        headers.set(line.slice(0, colon).trim().toLowerCase(), line.slice(colon + 1).trim());
    }
    return { method: requestLine[1], path: requestLine[2], headers };
}

// ### Frames ##################################################################

// One frame, FIN set: header, then the payload, masked with 'mask' (4 bytes)
// when given (a client must mask, a server must not).
export function encodeFrame(opcode, payload, mask) {
    // the length on 7 bits, or 126 + 16 bits, or 127 + 64 bits
    const length = payload.length;
    const extended = length < 126 ? 0 : length < 0x10000 ? 2 : 8;
    const maskSize = mask === undefined ? 0 : 4;
    const frame = new Uint8Array(2 + extended + maskSize + length);

    frame[0] = 0x80 | opcode;
    const maskBit = mask === undefined ? 0 : 0x80;
    if (extended === 0) {
        frame[1] = maskBit | length;
    } else if (extended === 2) {
        frame[1] = maskBit | 126;
        frame[2] = length >> 8;
        frame[3] = length & 0xff;
    } else {
        frame[1] = maskBit | 127;
        const view = new DataView(frame.buffer);
        view.setUint32(2, Math.floor(length / 0x100000000));
        view.setUint32(6, length >>> 0);
    }

    // the payload, masked or not
    const payloadStart = 2 + extended + maskSize;
    if (mask === undefined) {
        frame.set(payload, payloadStart);
        return frame;
    }

    frame.set(mask, 2 + extended);
    for (let i = 0; i < length; i++) {
        frame[payloadStart + i] = payload[i] ^ mask[i & 3];
    }
    return frame;
}

// Cuts a stream of bytes into frames: feed() the bytes as they come,
// onFrame({ fin, opcode, payload }) is called for each complete frame (the
// payload unmasked). Throws on a frame larger than MAX_MESSAGE.
export class FrameParser {
    constructor(onFrame) {
        this.onFrame = onFrame;
        this.pending = new Uint8Array(0);
    }

    feed(bytes) {
        // append to what is left of the previous calls
        const joined = new Uint8Array(this.pending.length + bytes.length);
        joined.set(this.pending);
        joined.set(bytes, this.pending.length);

        // take every complete frame
        let position = 0;
        for (;;) {
            const frame = readFrame(joined, position);
            if (frame === null) {
                break;
            }
            position = frame.end;
            this.onFrame({ fin: frame.fin, opcode: frame.opcode, payload: frame.payload });
        }
        this.pending = joined.slice(position);
    }
}

// The frame starting at 'start' of 'bytes', or null if it is not complete:
// { fin, opcode, payload, end }.
function readFrame(bytes, start) {
    if (bytes.length - start < 2) {
        return null;
    }

    // first two bytes: FIN, opcode, MASK, length on 7 bits
    const fin = (bytes[start] & 0x80) !== 0;
    const opcode = bytes[start] & 0x0f;
    const masked = (bytes[start + 1] & 0x80) !== 0;
    let length = bytes[start + 1] & 0x7f;
    let position = start + 2;

    // the extended length
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (length === 126) {
        if (bytes.length - position < 2) {
            return null;
        }
        length = view.getUint16(position);
        position += 2;
    } else if (length === 127) {
        if (bytes.length - position < 8) {
            return null;
        }
        length = view.getUint32(position) * 0x100000000 + view.getUint32(position + 4);
        position += 8;
    }

    if (length > MAX_MESSAGE) {
        throw new Error(`websocket: frame of ${length} bytes, too large`);
    }

    // the mask, then the payload
    const maskSize = masked ? 4 : 0;
    if (bytes.length - position < maskSize + length) {
        return null;
    }

    const mask = bytes.slice(position, position + maskSize);
    position += maskSize;
    const payload = bytes.slice(position, position + length);
    if (masked) {
        for (let i = 0; i < length; i++) {
            payload[i] ^= mask[i & 3];
        }
    }
    return { fin, opcode, payload, end: position + length };
}

// ### Connections #############################################################

// One WebSocket over a net connection, either side ('masking': true on the
// client side, which masks what it sends).
//   socket.onMessage(text => ...)   a whole text message (fragments joined)
//   socket.onClose(() => ...)
//   socket.send(text)
//   socket.close()
export class WebSocket {
    constructor(connection, { masking = false } = {}) {
        this.connection = connection;
        this.masking = masking;
        this.messageHandler = undefined;
        this.closeHandler = undefined;
        this.fragments = [];        // payloads of a message not finished yet
        this.closed = false;
        this.parser = new FrameParser(frame => this.onFrame(frame));

        connection.onData(bytes => this.onData(bytes));
        connection.onClose(() => this.onConnectionClosed());
    }

    onMessage(handler) {
        this.messageHandler = handler;
    }

    onClose(handler) {
        this.closeHandler = handler;
    }

    send(text) {
        this.sendFrame(OPCODE.TEXT, encoder.encode(text));
    }

    // Sends a close frame, then closes the connection.
    close() {
        if (this.closed) {
            return;
        }
        this.sendFrame(OPCODE.CLOSE, new Uint8Array(0));
        this.connection.close();
    }

    sendFrame(opcode, payload) {
        if (this.closed || !this.connection.isOpen()) {
            return;
        }
        const mask = this.masking ? randomMask() : undefined;
        this.connection.write(encodeFrame(opcode, payload, mask));
    }

    onData(bytes) {
        try {
            this.parser.feed(bytes);
        } catch (error) {
            // a frame too large: the peer is not a debugger we can serve
            this.connection.close();
        }
    }

    // One frame: a piece of a message, or a control frame.
    onFrame({ fin, opcode, payload }) {
        switch (opcode) {
        case OPCODE.TEXT:
        case OPCODE.BINARY:
        case OPCODE.CONTINUATION: {
            this.fragments.push(payload);
            if (fin) {
                const text = decoder.decode(joinBytes(this.fragments));
                this.fragments = [];
                this.messageHandler?.(text);
            }
            break;
        }
        case OPCODE.PING: {
            this.sendFrame(OPCODE.PONG, payload);
            break;
        }
        case OPCODE.CLOSE: {
            // answer the close, then end the connection
            this.sendFrame(OPCODE.CLOSE, payload.slice(0, 2));
            this.connection.close();
            break;
        }
        default: {
            // PONG, and opcodes this server does not know: ignored
            break;
        }
        }
    }

    onConnectionClosed() {
        if (this.closed) {
            return;
        }
        this.closed = true;
        this.closeHandler?.();
    }
}

function joinBytes(parts) {
    const size = parts.reduce((total, part) => total + part.length, 0);
    const joined = new Uint8Array(size);
    let offset = 0;
    for (const part of parts) {
        joined.set(part, offset);
        offset += part.length;
    }
    return joined;
}

// A mask for a client frame: RFC 6455 wants it unpredictable for proxies;
// Math.random is enough between a debugger and its local tool.
function randomMask() {
    const mask = new Uint8Array(4);
    for (let i = 0; i < 4; i++) {
        mask[i] = Math.floor(Math.random() * 256);
    }
    return mask;
}

// ### Server ##################################################################

// Starts the server. Throws if it cannot listen.
//   host, port      where to listen (port 0: any free port, see server.port())
//   routes(path)    the JSON answer of a GET request, undefined: 404
//   acceptsSocket(path)  true if a WebSocket may open on this path
//   onSocket(socket)     a WebSocket opened
// Returns { port(), close() }.
export function startServer({ host = "127.0.0.1", port, routes, acceptsSocket, onSocket }) {
    const server = listen({ host, port }, connection => {
        serveConnection(connection, { routes, acceptsSocket, onSocket });
    });
    return {
        port: () => server.port(),
        close: () => server.close(),
    };
}

// One connection: reads the head of the HTTP request, then answers it with
// JSON or upgrades it to a WebSocket.
function serveConnection(connection, { routes, acceptsSocket, onSocket }) {
    let received = "";
    let answered = false;

    connection.onData(bytes => {
        if (answered) {
            return;
        }

        // wait for the whole head (a debugger sends GET requests: no body)
        received += decoder.decode(bytes);
        const headEnd = received.indexOf("\r\n\r\n");
        if (headEnd < 0) {
            if (received.length > MAX_REQUEST_HEAD) {
                answered = true;
                connection.close();
            }
            return;
        }

        answered = true;
        const request = parseRequestHead(received.slice(0, headEnd));
        answerRequest(connection, request, { routes, acceptsSocket, onSocket });
    });
    connection.onClose(() => {});
}

// Answers one HTTP request: a WebSocket upgrade, a JSON route, or an error.
function answerRequest(connection, request, { routes, acceptsSocket, onSocket }) {
    if (request === null || request.method !== "GET") {
        writeResponse(connection, "400 Bad Request", "text/plain", "bad request\n");
        return;
    }

    // a WebSocket: the handshake, then the socket takes the connection over
    const isUpgrade = request.headers.get("upgrade")?.toLowerCase() === "websocket";
    if (isUpgrade) {
        const key = request.headers.get("sec-websocket-key");
        if (key === undefined || !acceptsSocket(request.path)) {
            writeResponse(connection, "404 Not Found", "text/plain", "no debugger here\n");
            return;
        }

        connection.write([
            "HTTP/1.1 101 Switching Protocols",
            "Upgrade: websocket",
            "Connection: Upgrade",
            `Sec-WebSocket-Accept: ${acceptKey(key)}`,
            "", "",
        ].join("\r\n"));
        onSocket(new WebSocket(connection));
        return;
    }

    // a JSON route
    const path = request.path.split("?")[0];
    const answer = routes(path);
    if (answer === undefined) {
        writeResponse(connection, "404 Not Found", "text/plain", "not found\n");
        return;
    }
    writeResponse(connection, "200 OK", "application/json; charset=UTF-8", JSON.stringify(answer, null, 2));
}

// Writes a whole HTTP response, then closes the connection.
function writeResponse(connection, status, contentType, body) {
    const bodyBytes = encoder.encode(body);
    const head = [
        `HTTP/1.1 ${status}`,
        `Content-Type: ${contentType}`,
        `Content-Length: ${bodyBytes.length}`,
        "Connection: close",
        "", "",
    ].join("\r\n");
    connection.write(head);
    connection.write(bodyBytes);
    connection.close();
}

// ### Client (tests) ##########################################################

// Opens a WebSocket to ws://host:port/path; resolves with a WebSocket (that
// masks its frames), rejects if the server refuses the upgrade.
export async function connect({ host = "127.0.0.1", port, path }) {
    const connection = await netConnect({ host, port });
    const key = new Uint8Array(16).map(() => Math.floor(Math.random() * 256)).toBase64();

    return new Promise((resolve, reject) => {
        let received = new Uint8Array(0);
        connection.onData(bytes => {
            // wait for the end of the head of the answer
            received = joinBytes([received, bytes]);
            const text = decoder.decode(received);
            const headEnd = text.indexOf("\r\n\r\n");
            if (headEnd < 0) {
                return;
            }

            // the answer must accept our key
            const head = text.slice(0, headEnd);
            const accepted = head.startsWith("HTTP/1.1 101") && head.includes(`Sec-WebSocket-Accept: ${acceptKey(key)}`);
            if (!accepted) {
                connection.close();
                reject(new Error(`websocket: upgrade refused: ${head.split("\r\n")[0]}`));
                return;
            }

            // the socket takes the connection over, with the bytes after the head
            const socket = new WebSocket(connection, { masking: true });
            const rest = received.slice(encoder.encode(text.slice(0, headEnd + 4)).length);
            if (rest.length > 0) {
                socket.onData(rest);
            }
            resolve(socket);
        });

        connection.write([
            `GET ${path} HTTP/1.1`,
            `Host: ${host}:${port}`,
            "Upgrade: websocket",
            "Connection: Upgrade",
            `Sec-WebSocket-Key: ${key}`,
            "Sec-WebSocket-Version: 13",
            "", "",
        ].join("\r\n"));
    });
}
