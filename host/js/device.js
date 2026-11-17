
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
import {
    KIND, KEY, PROTOCOL_VERSION, DEFAULT_MAX_MESSAGE, MIN_MAX_MESSAGE, encodeMessage, decodeMessage, kindName,
} from "qjsx:protocol";

const DEFAULT_TIMEOUT = 3000;
const DEFAULT_ATTEMPTS = 3;

/**
 * Target text -> { kind: "tcp", host, port } or { kind: "serial", path, baudRate }.
 */
export function parseTarget(text) {
    // tcp:<host>:<port>
    const tcp = /^tcp:(.+):(\d+)$/.exec(text);
    if (tcp) {
        return { kind: "tcp", host: tcp[1], port: Number(tcp[2]) };
    }

    // serial:<port>[@<baud rate>]
    const serial = /^serial:([^@]+)(?:@(\d+))?$/.exec(text);
    if (serial) {
        return { kind: "serial", path: serial[1], baudRate: Number(serial[2] ?? 115200) };
    }

    throw new Error(`invalid target '${text}' (expected tcp:<host>:<port> or serial:<port>[@<baud rate>])`);
}

/**
 * Error answered by the agent; 'code' is its Node-like code when it gave one
 * ("ENOENT"... for the files of the data folder), else undefined.
 */
export class DeviceError extends Error {
    /**
     * 'message' and 'code' as the agent gave them.
     */
    constructor(message, code) {
        super(message);
        this.name = "DeviceError";
        this.code = code;
    }
}

/**
 * A session with a device agent: requests, their answers, the events.
 */
export class Device {
    /**
     * Opens the link to a target (see parseTarget); the session itself is
     * opened by hello().
     * handlers: onLog(stream, text), onEvent(kind, payload), onText(bytes), onClose()
     */
    static async open(targetText, handlers = {}) {
        const target = parseTarget(targetText);

        // a TCP connection or a serial port
        let link;
        if (target.kind === "tcp") {
            link = await connect({ host: target.host, port: target.port });
        } else {
            link = new Serial(target.path, { baudRate: target.baudRate });
        }

        return new Device(link, handlers, target.kind === "serial");
    }

    /**
     * Takes an open link; use Device.open().
     */
    constructor(link, handlers, isSerial) {
        this.link = link;
        this.handlers = handlers;
        this.pending = new Map();   // id -> { resolve, reject, timer, attempts, frame, kind }
        this.nextId = 1;
        this.closed = false;
        this.maxMessage = undefined;   // largest message of the device (HELLO); undefined before HELLO

        // received bytes -> frames (messages) and console text
        this.parser = new FrameParser(message => this.onMessage(message), bytes => this.handlers.onText?.(bytes));
        link.onData(bytes => this.parser.feed(bytes));
        link.onClose(() => this.onLinkClosed());

        // Console text on a serial link has no frame delimiter: flush it when idle.
        this.textTimer = undefined;
        if (isSerial) {
            this.textTimer = setInterval(() => this.parser.flushText(), 100);
        }
    }

    /**
     * Opens a device that is restarting (after RESET): tries to open the
     * target and open the session again and again (the port of a native USB
     * link goes away during the restart, a TCP agent does not listen yet)
     * until it answers, at most 'timeout' ms. Resolves with the device, its
     * session open (hello done). The handlers are given to the device once
     * it answers (closing a failed try must not look like the end of the
     * connection to them).
     */
    static async reopen(targetText, handlers = {}, token = undefined, { timeout = 20000 } = {}) {
        const RETRY_DELAY = 300;   // ms between two tries
        const deadline = Date.now() + timeout;

        for (;;) {
            // one try: open the link and the session, without handlers
            let device;
            try {
                device = await Device.open(targetText, {});
                await device.hello(token, { timeout: 500, attempts: 1 });
                device.handlers = handlers;
                return device;
            } catch (error) {
                // failed: close what was opened, give up after the deadline
                device?.close();
                if (Date.now() >= deadline) {
                    throw new Error(`the device did not come back within ${timeout / 1000} s (${error.message})`);
                }
            }

            // wait a little before the next try
            await new Promise(resolve => setTimeout(resolve, RETRY_DELAY));
        }
    }

    /**
     * Opens the session. Keeps the native modules of the firmware in
     * 'natives', its qjsx version in 'version' (undefined if an older
     * firmware does not say) and its largest message in 'maxMessage' (64 KB
     * if it does not say). 'options' are those of request().
     */
    async hello(token, options = {}) {
        // HELLO with our protocol version and the token if any
        const payload = { [KEY.PROTOCOL]: PROTOCOL_VERSION };
        if (token !== undefined) {
            payload[KEY.TOKEN] = token;
        }

        // keep what the device says about itself
        const answer = await this.request(KIND.HELLO, payload, options);
        this.natives = answer.get(KEY.NATIVES);
        this.version = answer.get(KEY.VERSION);
        this.maxMessage = answer.get(KEY.MAX_MESSAGE) ?? DEFAULT_MAX_MESSAGE;

        // a device that takes only small messages cannot be used
        if (this.maxMessage < MIN_MAX_MESSAGE) {
            throw new DeviceError(`the device takes messages of ${this.maxMessage} bytes at most (${MIN_MAX_MESSAGE} needed)`);
        }

        return answer;
    }

    /**
     * Sends a request; resolves with the answer payload (a Map). A request
     * without answer is sent again with the same id (the agent answers a
     * repeated id without executing it twice). A request larger than the
     * device's largest message is refused here (the device would drop it
     * without a word): code "EMSGSIZE".
     */
    request(kind, payload = {}, { timeout = DEFAULT_TIMEOUT, attempts = DEFAULT_ATTEMPTS } = {}) {
        if (this.closed) {
            return Promise.reject(new Error("device: connection closed"));
        }

        // number and encode the message; refuse it when the device cannot take it
        const id = this.nextId++;
        const message = encodeMessage(kind, id, payload);
        const isTooLarge = this.maxMessage !== undefined && message.length > this.maxMessage;
        if (isTooLarge) {
            return Promise.reject(new DeviceError(
                `${kindName(kind)}: ${message.length} bytes, more than the device takes (${this.maxMessage})`, "EMSGSIZE"));
        }

        // send it; its answer settles the promise (onMessage)
        const frame = encodeFrame(message);
        return new Promise((resolve, reject) => {
            const entry = { resolve, reject, timer: undefined, attempts, frame, kind, timeout };
            this.pending.set(id, entry);
            this.send(id, entry);
        });
    }

    /**
     * Sends the frame of a pending request, and sends it again after its
     * timeout while attempts are left; the last timeout rejects it.
     */
    send(id, entry) {
        // one attempt more, with its timer
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

    /**
     * A message received: the answer of a pending request, a LOG, or another event.
     */
    onMessage(bytes) {
        // decode it; anything else is ignored
        let message;
        try {
            message = decodeMessage(bytes);
        } catch (error) {
            return;   // not a message of this protocol
        }

        // an answer settles its request
        const { kind, id, payload } = message;
        const isAnswer = kind === KIND.RESPONSE || kind === KIND.ERROR;
        if (isAnswer) {
            const entry = this.pending.get(id);
            if (entry === undefined) {
                return;   // answer to a request already answered (repeated)
            }

            this.pending.delete(id);
            clearTimeout(entry.timer);
            if (kind === KIND.RESPONSE) {
                entry.resolve(payload ?? new Map());
            } else {
                const text = payload?.get(KEY.MESSAGE) ?? "error";
                const code = payload?.get(KEY.ERROR_CODE);
                entry.reject(new DeviceError(text, code));
            }

            return;
        }

        // the output of the program
        if (kind === KIND.LOG) {
            this.handlers.onLog?.(payload.get(KEY.STREAM), payload.get(KEY.TEXT));
            return;
        }

        // any other event
        this.handlers.onEvent?.(kind, payload);
    }

    /**
     * The link closed: every pending request is rejected.
     */
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

    /**
     * Closes the link (onLinkClosed follows).
     */
    close() {
        clearInterval(this.textTimer);
        this.link.close();
    }
}
