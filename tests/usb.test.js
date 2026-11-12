/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// device/usb and stdlib/usb in the digital twin ("qjsx agent --board
// yd-esp32-s3 --usb-port <port>"), whose serial functions are TCP ports: a
// TCP client plays the host (a tablet). Program: tests/fixtures/twinusb.
//
//   qjsx run tests/usb.test.js tcp:127.0.0.1:<agent port> <usb port>
import * as fs from "sys/fs";
import { connect as netConnect } from "sys/net";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND } from "qjsx:protocol";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const USB_PORT = Number(process.argv[3]);
const PROJECT = "tests/fixtures/twinusb";
const WAIT_TIMEOUT = 5000;

// --- the program's output, line by line
const lines = [];
const lineWaiters = [];

// How many times the program printed 'line'.
function timesPrinted(line) {
    return lines.filter(item => item === line).length;
}

function onLog(stream, text) {
    for (const line of text.split("\n").filter(item => item !== "")) {
        lines.push(line);
        for (const waiter of lineWaiters.filter(item => timesPrinted(item.line) >= item.times)) {
            lineWaiters.splice(lineWaiters.indexOf(waiter), 1);
            clearTimeout(waiter.timer);
            waiter.resolve(true);
        }
    }
}

// Resolves true when the program has printed 'line' 'times' times in all,
// false after WAIT_TIMEOUT.
function printed(line, times = 1) {
    if (timesPrinted(line) >= times) {
        return Promise.resolve(true);
    }
    return new Promise(resolve => {
        const waiter = { line, times, resolve, timer: undefined };
        waiter.timer = setTimeout(() => {
            lineWaiters.splice(lineWaiters.indexOf(waiter), 1);
            resolve(false);
        }, WAIT_TIMEOUT);
        lineWaiters.push(waiter);
    });
}

// --- the host side: a TCP client on the serial function
class Host {
    constructor(connection) {
        this.connection = connection;
        this.text = "";
        this.closed = false;
        this.waiter = null;
        connection.onData(bytes => {
            this.text += new TextDecoder().decode(bytes);
            this.wake();
        });
        connection.onClose(() => {
            this.closed = true;
            this.wake();
        });
    }

    static async open() {
        return new Host(await netConnect({ port: USB_PORT }));
    }

    wake() {
        if (this.waiter !== null && this.waiter.done()) {
            clearTimeout(this.waiter.timer);
            this.waiter.resolve(true);
            this.waiter = null;
        }
    }

    // Resolves true once done() holds, false after WAIT_TIMEOUT.
    until(done) {
        if (done()) {
            return Promise.resolve(true);
        }
        return new Promise(resolve => {
            const timer = setTimeout(() => {
                this.waiter = null;
                resolve(false);
            }, WAIT_TIMEOUT);
            this.waiter = { done, resolve, timer };
        });
    }

    // Sends 'text' and resolves with what came back once 'expected' has
    // arrived (or after WAIT_TIMEOUT).
    async exchange(text, expected) {
        this.text = "";
        this.connection.write(text);
        await this.until(() => this.text.includes(expected));
        return this.text;
    }

    close() {
        this.connection.close();
    }
}

// Resolves after 'milliseconds'.
function pause(milliseconds) {
    return new Promise(resolve => setTimeout(resolve, milliseconds));
}

// --- the agent: the twin offers device/usb
const device = await Device.open(TARGET, { onLog });
await device.hello();
check(device.natives.includes("device/usb"), "the twin offers device/usb");

// --- the program: its errors, then the device started
const projectBuild = build(PROJECT);
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(projectBuild.file));
await device.request(KIND.RUN);
check(await printed("usb ready"), "the program started its USB device");
check(lines.includes("bad text: RangeError"), "a descriptor text that is not ASCII is refused");
check(lines.includes("serial after start: usb: already started"), "no function after start()");
check(lines.some(line => line.startsWith("second device: usb: busy")), "one Usb object at a time");
check(lines.includes("line false false"), "no host yet: DTR and RTS at 0");

// --- a host opens the port, sends, closes
let host = await Host.open();
check(await printed("line state true true"), "a host opening the port raises DTR and RTS");
equal(await host.exchange("hello\n", "HELLO\n"), "HELLO\n", "bytes go both ways");
host.close();
check(await printed("line state false false"), "the host closing the port lowers DTR and RTS");

// --- another program takes the started device over: the host stays
host = await Host.open();
check(await printed("line state true true", 2), "a host opens the port again");
await device.request(KIND.STOP);
await pause(300);
check(!host.closed, "the end of the program leaves the USB device to the host");

await device.request(KIND.RUN);
check(await printed("usb ready", 2), "the program runs again");
check(lines.includes("line true true"), "it takes the device over, the host still there");
equal(await host.exchange("again\n", "AGAIN\n"), "AGAIN\n", "the host talks to the new program");

// --- close() stops the device: the host sees it go away
equal(await host.exchange("close\n", "bye\n"), "bye\n", "the program answers before closing");
check(await host.until(() => host.closed), "close() stops the device: the host is disconnected");
check(await printed("closed"), "the program closed its device");

let refused = false;
try {
    const late = await netConnect({ port: USB_PORT });
    late.close();
} catch {
    refused = true;
}
check(refused, "a stopped device has no port any more");

await device.request(KIND.STOP);
device.close();
report("usb");
