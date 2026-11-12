/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A program for the USB device of the digital twin (tests/usb.test.js): the
// errors of device/usb, then a serial function that sends back what it gets
// in capitals, until the host sends "close".
import { Usb, UsbSerial } from "stdlib/usb";

// a text the USB descriptors cannot carry
try {
    new Usb({ product: "café" });
    console.log("bad text: accepted");
} catch (error) {
    console.log(`bad text: ${error.name}`);
}

// the device, started at once
const port = new UsbSerial({ manufacturer: "qjsx tests", product: "echo", serialNumber: "42" });

// no function after start(), one device at a time
try {
    port.serial();
} catch (error) {
    console.log(`serial after start: ${error.message}`);
}
try {
    new Usb();
} catch (error) {
    console.log(`second device: ${error.message}`);
}

// the line now (the host may already be there: a device taken over)
const now = port.lineState();
console.log(`line ${now.dtr} ${now.rts}`);

port.onLineState(state => {
    console.log(`line state ${state.dtr} ${state.rts}`);
});

// the echo, in capitals; "close" stops the device
port.onData(bytes => {
    const text = new TextDecoder().decode(bytes);
    if (text.startsWith("close")) {
        port.write("bye\n");
        port.close();
        console.log("closed");
        return;
    }
    port.write(text.toUpperCase());
});

console.log("usb ready");
