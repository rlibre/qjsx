/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The web page of the digital twin ("qjsx agent --board <name> --ui-port
// <port>"): runs in a JS runtime of its own, on a thread of the agent, next
// to the device program. It reads the simulated board through the native
// module "qjsx:twinboard" (host/twin/sim/twin_module.cpp) and serves:
//   GET /            the page (host/twin/ui, an x4js application), and the
//   GET /<file>      files of its build: the zip embedded in the executable,
//                    or the folder given by the hidden option --ui-dir
//   GET /board.json  the board description
//   GET /state       the state now (pins, UARTs, USB), as below
//   GET /parts/<id>/view.js   the view of its own of a part of the project
//   WebSocket /ws    the x4js global messages of the page (subprotocol
//                    "messaging"): JSON texts { msg, params }
//
// It also builds the parts (qjsx:twin/parts: the board's and the project's),
// gives them the events of the simulated hardware (twinboard.Bridge), then
// tells the agent the twin is ready (twinboard.ready), or why not.
//
// Messages (host/twin/ui/src/messages.ts):
//   twin.board      twin -> page   the board description, when a page connects
//   twin.parts      twin -> page   the parts: [{ id, label, model, controls, view, pins }],
//                                  when a page connects
//   twin.part.state twin -> page   { id, state }: what a part published (the last
//                                  of each part when a page connects)
//   twin.part.action page -> twin  { id, action, args }: to a part (onAction)
//   twin.log        twin -> page   { id, text }: a line of a part (part.log, or an
//                                  exception of its model; id null if unknown);
//                                  the last KEPT_LINES when a page connects
//   twin.state      twin -> page   the state, when it changes (every POLL_MS at most)
//   twin.pin.force  page -> twin   { pin, level: 0 | 1 | -1 (let go) }
//   twin.uart.data  twin -> page   { uart, records: [{ sequence, source, bytes }] }:
//                                  the traffic of a UART (source "program": what it
//                                  wrote; "page", "tcp": what it received from
//                                  them; bytes: an array of numbers); a page that
//                                  connects gets what is kept, then the new records
//   twin.uart.send  page -> twin   { uart, bytes: [numbers] }: bytes for the program
//   twin.i2c.data   twin -> page   { records: [{ sequence, time, bus, address, status,
//                                  writeSize, readSize, written, read }] }: the I2C
//                                  transfers of the program (bytes as arrays of numbers;
//                                  the first 32 of each); a page that connects gets
//                                  what is kept, then the new records
//   twin.tcp.listen page -> twin   { kind: "uart" | "usb", index, port, listen }: the
//                                  TCP link of a UART or of a USB serial function
//                                  (the twin keeps the choice in its storage folder)
//
// The state: { pins: [{ configured, mode, pull, level, forced, partDrive, partPull,
//                       conflict, rmt }],
//              uarts: [{ number, link, open, tcp, baudRate, dataBits, parity,
//                        stopBits, flowControl, tx, rx, rts, cts }],
//              usb: { started, manufacturer, product, serialNumber, vendorId,
//                     productId, serials, functions: [{ tcp }] },
//              i2c: [{ bus, open, sda, scl, parts: [{ address, id }] }] }
// where tcp = { port, listen, listening, client, error }.
//
// Silent: what this runtime prints would go where the device program's
// output goes (one output sink per process). The agent prints the address.
import * as fs from "sys/fs";
import { startServer } from "qjsx:websocket";
import { readZip } from "qjsx:zip";
import { loadParts } from "qjsx:twin/parts";
import * as twinboard from "qjsx:twinboard";

// How often the state is read for the open pages, in ms.
const POLL_MS = 50;

// Lines of the parts (twin.log) kept for a page that connects later.
const KEPT_LINES = 200;

// Content type of the files of the page, by extension.
const CONTENT_TYPES = {
    html: "text/html; charset=UTF-8",
    js: "text/javascript; charset=UTF-8",
    css: "text/css; charset=UTF-8",
    json: "application/json; charset=UTF-8",
    map: "application/json; charset=UTF-8",
    svg: "image/svg+xml",
    png: "image/png",
    jpg: "image/jpeg",
    jpeg: "image/jpeg",
    woff: "font/woff",
    woff2: "font/woff2",
    ttf: "font/ttf",
};

// The I2C buses now, each with the parts wired on its pins: [{ bus, open,
// sda, scl, parts: [{ address, id }] }] ('board': the board of the parts).
function readI2c(board) {
    return twinboard.i2cBuses().map(bus => {
        const wired = bus.open ? board._buses.get(`${bus.sda}/${bus.scl}`) : undefined;
        const parts = [...(wired?._parts ?? [])].map(([address, id]) => ({ address, id }));
        return { ...bus, parts };
    });
}

// The state of the board now (see the top of the file).
function readState(board) {
    return { pins: twinboard.pins(), uarts: twinboard.uarts(), usb: twinboard.usb(), i2c: readI2c(board) };
}

// The twin.i2c.data message of records of the I2C trace (bytes as arrays of
// numbers).
function i2cMessage(records) {
    const params = {
        records: records.map(record => ({ ...record, written: Array.from(record.written), read: Array.from(record.read) })),
    };
    return JSON.stringify({ msg: "twin.i2c.data", params });
}

// The parts the project wires on the board: the "parts" of the "twin"
// section of its qjsx.json (none without a project).
function projectParts(projectDir) {
    if (projectDir === undefined) {
        return [];
    }
    const settings = JSON.parse(fs.readFileSync(`${projectDir}/qjsx.json`, "utf8"));
    const parts = settings.twin?.parts ?? [];
    if (!Array.isArray(parts)) {
        throw new Error('qjsx.json: "twin.parts" must be an array');
    }
    return parts;
}

// The largest record number: asking for the records after it gives none.
const LAST_RECORD = 4294967295;

// The twin.uart.data message of records of a UART (bytes as arrays of numbers).
function trafficMessage(uart, records) {
    const params = {
        uart,
        records: records.map(record => ({
            sequence: record.sequence,
            source: record.source,
            bytes: Array.from(record.bytes),
        })),
    };
    return JSON.stringify({ msg: "twin.uart.data", params });
}

// twin.tcp.listen: the port and the listening of a TCP link.
function setTcpLink(params) {
    const { kind, index, port, listen } = params ?? {};
    const validKind = kind === "uart" || kind === "usb";
    const validPort = Number.isInteger(port) && port >= 0 && port <= 65535;
    if (validKind && Number.isInteger(index) && validPort && typeof listen === "boolean") {
        twinboard.tcpListen(`${kind}${index}`, port, listen);
    }
}

// True if 'bytes' is an array of numbers 0..255.
function isByteArray(bytes) {
    return Array.isArray(bytes) && bytes.every(byte => Number.isInteger(byte) && byte >= 0 && byte <= 255);
}

// twin.uart.send: bytes for the program on a UART (dropped if it is closed).
function sendToUart(params) {
    const { uart, bytes } = params ?? {};
    if (Number.isInteger(uart) && uart >= 0 && uart < twinboard.uarts().length && isByteArray(bytes)) {
        twinboard.uartSend(uart, new Uint8Array(bytes));
    }
}

// twin.pin.force: a pin forced to 0 or 1, or let go.
function forcePin(params) {
    const { pin, level } = params ?? {};
    const validLevel = level === 0 || level === 1 || level === -1;
    if (Number.isInteger(pin) && validLevel) {
        twinboard.force(pin, level);
    }
}

// A message of a page, to the twin or to a part ('actOnPart(params)' for
// twin.part.action). Others are ignored (the page sends every global message
// it fires).
function onPageMessage(text, actOnPart) {
    // a JSON message { msg, params }
    let message;
    try {
        message = JSON.parse(text);
    } catch {
        return;
    }

    switch (message?.msg) {
    case "twin.uart.send": {
        sendToUart(message.params);
        break;
    }
    case "twin.tcp.listen": {
        setTcpLink(message.params);
        break;
    }
    case "twin.pin.force": {
        forcePin(message.params);
        break;
    }
    case "twin.part.action": {
        actOnPart(message.params ?? {});
        break;
    }
    default: {
        break;
    }
    }
}

// The files of the page: name (without the leading "/") -> Uint8Array, or
// undefined. From the embedded zip, or read from 'uiDir' at each request
// (to work on the page without building qjsx again).
function pageFiles(uiDir) {
    // the folder of the page, read at each request
    if (uiDir !== undefined) {
        return name => {
            const file = `${uiDir}/${name}`;
            const exists = !name.split("/").includes("..") && fs.existsSync(file);
            return exists ? fs.readFileSync(file) : undefined;
        };
    }

    // the embedded zip, read once
    const entries = readZip(twinboard.uiArchive());
    return name => entries.get(name);
}

// The answer to GET 'path' for a file of the page, or undefined. The view of
// its own of a project's part ('viewFiles': id -> file) is read at each
// request.
function pageAnswer(files, viewFiles, path) {
    // a view of a part: /parts/<id>/view.js
    const view = /^\/parts\/([a-z0-9_-]+)\/view\.js$/.exec(path);
    if (view !== null) {
        const file = viewFiles.get(view[1]);
        return file !== undefined ? { contentType: CONTENT_TYPES.js, body: fs.readFileSync(file) } : undefined;
    }

    // a file of the page
    const name = path === "/" ? "index.html" : path.slice(1);
    const body = files(name);
    if (body === undefined) {
        return undefined;
    }

    const extension = name.slice(name.lastIndexOf(".") + 1).toLowerCase();
    const contentType = CONTENT_TYPES[extension] ?? "application/octet-stream";
    return { contentType, body };
}

// The text of an error of a model, for the console of the page.
function errorText(error) {
    return error instanceof Error ? `${error.name}: ${error.message}` : String(error);
}

/**
 * Serves the page of the twin on host:port, until the process ends:
 * 1. builds the parts (the board's, the project's), then the bridge that
 *    gives them the hardware events;
 * 2. starts the server; tells the agent it is ready (or why not: the agent
 *    stops, and so does this runtime);
 * 3. pushes the traffic of the UARTs and the state to the pages.
 */
export async function serveTwin({ host = "127.0.0.1", port, projectDir, uiDir }) {
    const board = JSON.parse(twinboard.board());
    const files = pageFiles(uiDir);
    const sockets = new Set();

    // to every open page
    function broadcast(msg, params) {
        const text = JSON.stringify({ msg, params });
        for (const entry of sockets) {
            entry.socket.send(text);
        }
    }

    // the parts: what they publish and their last lines are kept (for the
    // pages to come) and sent
    const lastStates = new Map();   // id -> state
    const keptLines = [];           // the last KEPT_LINES of twin.log
    function addLine(line) {
        keptLines.push(line);
        if (keptLines.length > KEPT_LINES) {
            keptLines.shift();
        }
        broadcast("twin.log", line);
    }
    const hooks = {
        published: (part, state) => {
            lastStates.set(part.id, state);
            broadcast("twin.part.state", { id: part.id, state });
        },
        logged: (part, text) => addLine({ id: part.id, text }),
        failed: (id, error) => addLine({ id: id ?? null, text: errorText(error) }),
    };

    let parts;
    try {
        parts = await loadParts({ boardParts: board.parts, projectParts: projectParts(projectDir), projectDir, hooks });
    } catch (error) {
        twinboard.ready(error.message);
        return;
    }

    const partsById = new Map(parts.parts.map(entry => [entry.part.id, entry]));
    const viewFiles = new Map(parts.parts.filter(entry => entry.viewFile).map(entry => [entry.part.id, entry.viewFile]));
    const descriptions = parts.parts.map(entry => entry.description);
    new twinboard.Bridge(event => parts.board._dispatch(event));

    // twin.part.action: to the part (an exception goes to the console)
    function actOnPart({ id, action, args }) {
        const entry = partsById.get(id);
        if (entry === undefined || typeof action !== "string") {
            return;
        }
        try {
            entry.part.onAction(action, args);
        } catch (error) {
            hooks.failed(id, error);
        }
    }

    // per UART, the number of the last record sent to the pages
    const uartCount = twinboard.uarts().length;
    const sentRecords = new Array(uartCount).fill(0);

    // the number of the last record of the I2C trace sent to the pages
    let sentI2c = 0;

    try {
        startServer({
            host,
            port,
            pages: path => pageAnswer(files, viewFiles, path),
            routes: path => {
                if (path === "/board.json") {
                    return board;
                }
                if (path === "/state") {
                    return readState(parts.board);
                }
                return undefined;
            },
            protocols: ["messaging"],
            acceptsSocket: path => path === "/ws",
            onSocket: socket => {
                // a new page gets the board, the parts, their states and their
                // last lines at once, the state of the board at the next poll
                const entry = { socket, last: "" };
                sockets.add(entry);
                socket.send(JSON.stringify({ msg: "twin.board", params: board }));
                socket.send(JSON.stringify({ msg: "twin.parts", params: descriptions }));
                for (const [id, state] of lastStates) {
                    socket.send(JSON.stringify({ msg: "twin.part.state", params: { id, state } }));
                }
                for (const line of keptLines) {
                    socket.send(JSON.stringify({ msg: "twin.log", params: line }));
                }
                sendKeptTraffic(socket);
                sendKeptI2c(socket);
                socket.onMessage(text => onPageMessage(text, actOnPart));
                socket.onClose(() => sockets.delete(entry));
            },
        });
    } catch (error) {
        twinboard.ready(`the page of the twin cannot listen on ${host}:${port}: ${error.message}`);
        return;
    }

    twinboard.ready();

    // a new page: the records kept of each UART, up to those the other pages
    // got (the newer ones come with the next poll, to every page)
    function sendKeptTraffic(socket) {
        for (let uart = 0; uart < uartCount; uart++) {
            const { records } = twinboard.uartTraffic(uart, 0);
            const kept = records.filter(record => record.sequence <= sentRecords[uart]);
            if (kept.length > 0) {
                socket.send(trafficMessage(uart, kept));
            }
        }
    }

    // the new records of each UART, to every page (none open: only skipped)
    function pushTraffic() {
        for (let uart = 0; uart < uartCount; uart++) {
            if (sockets.size === 0) {
                sentRecords[uart] = twinboard.uartTraffic(uart, LAST_RECORD).sequence;
                continue;
            }
            const { sequence, records } = twinboard.uartTraffic(uart, sentRecords[uart]);
            sentRecords[uart] = sequence;
            if (records.length === 0) {
                continue;
            }
            const text = trafficMessage(uart, records);
            for (const entry of sockets) {
                entry.socket.send(text);
            }
        }
    }

    // a new page: the I2C trace up to what the other pages got
    function sendKeptI2c(socket) {
        const kept = twinboard.i2cTrace(0).records.filter(record => record.sequence <= sentI2c);
        if (kept.length > 0) {
            socket.send(i2cMessage(kept));
        }
    }

    // the new records of the I2C trace, to every page (none open: only skipped)
    function pushI2c() {
        const { sequence, records } = twinboard.i2cTrace(sockets.size === 0 ? LAST_RECORD : sentI2c);
        sentI2c = sequence;
        if (records.length === 0) {
            return;
        }
        const text = i2cMessage(records);
        for (const entry of sockets) {
            entry.socket.send(text);
        }
    }

    // push the traffic, and the state when it changed, to the pages
    setInterval(() => {
        pushTraffic();
        pushI2c();
        if (sockets.size === 0) {
            return;
        }
        const text = JSON.stringify({ msg: "twin.state", params: readState(parts.board) });
        for (const entry of sockets) {
            if (entry.last !== text) {
                entry.last = text;
                entry.socket.send(text);
            }
        }
    }, POLL_MS);

    // the server and the timer keep this runtime alive
    await new Promise(() => {});
}
