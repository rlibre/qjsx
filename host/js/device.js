/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Connection to a device agent (an ESP, or "qjsx agent" on the desktop).
//
//   const device = await Device.open("tcp:127.0.0.1:5555", { onLog, onEvent, onText });
//   const info = await device.hello();
//   const result = await device.request(KIND.INFO);
//   device.close();
//
// Targets:  tcp:<host>:<port>   serial:<port>[@<baud rate>]   (COM3, /dev/ttyUSB0...)
import { connect } from "sys/net";
import { Serial } from "device/serial";
import { encodeFrame, FrameParser } from "qjsx:frames";
import { KIND, KEY, PROTOCOL_VERSION, encodeMessage, decodeMessage, kindName } from "qjsx:protocol";

const DEFAULT_TIMEOUT = 3000;
const DEFAULT_ATTEMPTS = 3;

export function parseTarget(text) {
    const tcp = /^tcp:(.+):(\d+)$/.exec(text);
    if (tcp) {
        return { kind: "tcp", host: tcp[1], port: Number(tcp[2]) };
    }
    const serial = /^serial:([^@]+)(?:@(\d+))?$/.exec(text);
    if (serial) {
        return { kind: "serial", path: serial[1], baudRate: Number(serial[2] ?? 115200) };
    }
    throw new Error(`invalid target '${text}' (expected tcp:<host>:<port> or serial:<port>[@<baud rate>])`);
}

// Error answered by the agent.
export class DeviceError extends Error {
    constructor(message) {
        super(message);
        this.name = "DeviceError";
    }
}

export class Device {
    // handlers: onLog(stream, text), onEvent(kind, payload), onText(bytes), onClose()
    static async open(targetText, handlers = {}) {
        const target = parseTarget(targetText);
        const link = target.kind === "tcp"
            ? await connect({ host: target.host, port: target.port })
            : new Serial(target.path, { baudRate: target.baudRate });
        return new Device(link, handlers, target.kind === "serial");
    }

    constructor(link, handlers, isSerial) {
        this.link = link;
        this.handlers = handlers;
        this.pending = new Map();   // id -> { resolve, reject, timer, attempts, frame, kind }
        this.nextId = 1;
        this.closed = false;
        this.parser = new FrameParser(message => this.onMessage(message), bytes => this.handlers.onText?.(bytes));
        link.onData(bytes => this.parser.feed(bytes));
        link.onClose(() => this.onLinkClosed());
        // Console text on a serial link has no frame delimiter: flush it when idle.
        this.textTimer = isSerial ? setInterval(() => this.parser.flushText(), 100) : undefined;
    }

    // Opens the session. Keeps the native modules of the firmware in
    // 'natives' and its qjsx version in 'version' (undefined if an older
    // firmware does not say).
    async hello(token) {
        const payload = { [KEY.PROTOCOL]: PROTOCOL_VERSION };
        if (token !== undefined) {
            payload[KEY.TOKEN] = token;
        }
        const answer = await this.request(KIND.HELLO, payload);
        this.natives = answer.get(KEY.NATIVES);
        this.version = answer.get(KEY.VERSION);
        return answer;
    }

    // Sends a request; resolves with the answer payload (a Map). A request
    // without answer is sent again with the same id (the agent answers a
    // repeated id without executing it twice).
    request(kind, payload = {}, { timeout = DEFAULT_TIMEOUT, attempts = DEFAULT_ATTEMPTS } = {}) {
        if (this.closed) {
            return Promise.reject(new Error("device: connection closed"));
        }
        const id = this.nextId++;
        const frame = encodeFrame(encodeMessage(kind, id, payload));
        return new Promise((resolve, reject) => {
            const entry = { resolve, reject, timer: undefined, attempts, frame, kind, timeout };
            this.pending.set(id, entry);
            this.send(id, entry);
        });
    }

    send(id, entry) {
        entry.attempts--;
        entry.timer = setTimeout(() => {
            if (entry.attempts > 0) {
                this.send(id, entry);
                return;
            }
            this.pending.delete(id);
            entry.reject(new Error(`device: no answer to ${kindName(entry.kind)}`));
        }, entry.timeout);
        this.link.write(entry.frame);
    }

    onMessage(bytes) {
        let message;
        try {
            message = decodeMessage(bytes);
        } catch (error) {
            return;   // not a message of this protocol
        }
        const { kind, id, payload } = message;
        if (kind === KIND.RESPONSE || kind === KIND.ERROR) {
            const entry = this.pending.get(id);
            if (entry === undefined) {
                return;   // answer to a request already answered (repeated)
            }
            this.pending.delete(id);
            clearTimeout(entry.timer);
            if (kind === KIND.RESPONSE) {
                entry.resolve(payload ?? new Map());
            } else {
                entry.reject(new DeviceError(payload?.get(KEY.MESSAGE) ?? "error"));
            }
            return;
        }
        if (kind === KIND.LOG) {
            this.handlers.onLog?.(payload.get(KEY.STREAM), payload.get(KEY.TEXT));
            return;
        }
        this.handlers.onEvent?.(kind, payload);
    }

    onLinkClosed() {
        this.closed = true;
        clearInterval(this.textTimer);
        for (const [, entry] of this.pending) {
            clearTimeout(entry.timer);
            entry.reject(new Error("device: connection closed"));
        }
        this.pending.clear();
        this.handlers.onClose?.();
    }

    close() {
        clearInterval(this.textTimer);
        this.link.close();
    }
}
