/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The messages between the page and the twin (host/twin/js/server.js). They
// are x4js global messages ({ msg, params }), carried over the WebSocket /ws
// by Application.setupSocketMessaging: a view fires or watches a message, it
// never sees the socket.
//
//   twin.board      twin -> page   the board description (BoardInfo)
//   twin.state      twin -> page   the state now (TwinState), when it changes
//   twin.pin.force  page -> twin   { pin, level: 0 | 1 | -1 (let go) }
//   twin.uart.data  twin -> page   { uart, records }: the traffic of a UART (UartRecord)
//   twin.uart.send  page -> twin   { uart, bytes }: bytes for the program
//   twin.tcp.listen page -> twin   { kind, index, port, listen }: the TCP link of a UART
//                                  or of a USB serial function (kept by the twin)
//   twin.parts      twin -> page   the parts on the board (PartInfo[])
//   twin.part.state twin -> page   { id, state }: what a part published
//   twin.part.action page -> twin  { id, action, args }: to a part
//   twin.log        twin -> page   { id, text }: a line of a part, or an error of its model
//   twin.i2c.data   twin -> page   { records }: the I2C transfers of the program (I2cRecord)
//   twin.scope.data twin -> page   what the pins did since the last message (ScopeData),
//                                  every 50 ms, for the analyzer
import { Application } from "x4js";

export const MSG = {
    board:    "twin.board",
    state:    "twin.state",
    forcePin: "twin.pin.force",
    uartData: "twin.uart.data",
    uartSend: "twin.uart.send",
    tcpListen: "twin.tcp.listen",
    parts:     "twin.parts",
    partState: "twin.part.state",
    partAction: "twin.part.action",
    log:       "twin.log",
    i2cData:   "twin.i2c.data",
    scopeData: "twin.scope.data",
};

// What a TCP link is for: a UART, or a serial function of the USB device.
export type TcpKind = "uart" | "usb";

// The TCP link of a UART or of a USB serial function.
export interface TcpState {
    port:      number;          // the port chosen; 0: none yet
    listen:    boolean;         // listening asked
    listening: boolean;         // listening now
    client:    boolean;         // a client is connected
    error:     string;          // why the last listening failed; "": none
}

// A part of the board description (boards/<name>.json): its model and its
// own fields, which the twin reads (the page gets the parts by twin.parts).
export interface BoardPart {
    id:     string;
    model:  string;
    label?: string;
}

// The board description (boards/<name>.json).
export interface BoardInfo {
    name:         string;
    chip:         string;
    description?: string;
    platform:     string;
    pins:         number;       // GPIO count
    natives:      string[];     // native modules of its firmware
    parts?:       BoardPart[];
    uarts?:       number;       // UARTs of the board (device/serial)
    linkUart?:    number;       // the UART of the agent's link on the real board: never opened
    adc?:         AdcInfo;      // its analog inputs
}

// The analog inputs of a board ("adc" of boards/<name>.json).
export interface AdcInfo {
    pins:  number[];            // the pins with an analog input
    bits:  number;              // resolution of the converter
    spans: number[];            // the ranges it reads, in mV
    vref:  number;              // volts: the level 1 of a pin
}

// One pin now.
export interface PinState {
    configured: boolean;        // the program configured it
    mode:       string;         // "input", "output", "openDrain"
    pull:       string;         // "none", "up", "down"
    level:      number;         // 0 or 1 on the line
    forced:     number | null;  // what the page forces, null: nothing
    partDrive:  number | null;  // what a part drives, null: nothing
    partPull:   string;         // a part's pull: "none", "up", "down"
    conflict:   boolean;        // the program and a part drive different levels
    rmt:        boolean;        // a channel of device/rmt drives it
    i2c:        boolean;        // a bus of device/i2c is open on it
    adc:        boolean;        // the program has its analog input open (device/adc)
    voltage:    number;         // the voltage on it, in volts
}

// One UART now (the settings are those of its last open).
export interface UartState {
    number:      number;
    link:        boolean;       // the agent's link of the real board
    open:        boolean;       // the program has it open
    tcp:         TcpState;      // its TCP link
    baudRate:    number;
    dataBits:    number;
    parity:      string;        // "none", "even", "odd"
    stopBits:    number;
    flowControl: boolean;
    tx:          number | null; // pins; null: not wired
    rx:          number | null;
    rts:         number | null;
    cts:         number | null;
}

// A record of the traffic of a UART.
export interface UartRecord {
    sequence: number;           // its number, per UART
    source:   string;           // "program" (it wrote), "page" or "tcp" (it received from them)
    bytes:    number[];
}

// The traffic of a UART (twin.uart.data).
export interface UartData {
    uart:    number;
    records: UartRecord[];
}

// The USB device now (the descriptors are those of its last start).
export interface UsbState {
    started:      boolean;
    manufacturer: string;
    product:      string;
    serialNumber: string;
    vendorId:     number;
    productId:    number;
    serials:      number;       // serial functions of the started device; 0: stopped
    functions:    { tcp: TcpState }[];   // every possible function, started or not
}

// The state of the twin now.
export interface TwinState {
    pins:  PinState[];
    uarts: UartState[];
    usb:   UsbState;
    i2c:   I2cBusState[];
}

// An I2C bus now, with the parts wired on its pins.
export interface I2cBusState {
    bus:   number;
    open:  boolean;             // the program has it open
    sda:   number | null;       // its pins (open)
    scl:   number | null;
    frequencies: number[];      // of the devices the program made on it, in Hz
    parts: { address: number; id: string }[];
}

// A transfer of the program on an I2C bus (a probe writes and reads nothing).
export interface I2cRecord {
    sequence:  number;          // its number, every bus together
    time:      number;          // ms since the twin started
    bus:       number;
    address:   number;
    status:    "ok" | "nack" | "timeout" | "failed";
    frequency: number;          // of the device, in Hz; 0: a probe
    writeSize: number;          // bytes written (the first 32 in written)
    readSize:  number;          // bytes asked (the first 32 of the answer in read)
    written:   number[];
    read:      number[];
}

// What a pin did since the last twin.scope.data.
export interface ScopeChange {
    pin:          number;
    records:      number[];     // its first changes: [time (µs), level, millivolts, ...]
    dropped:      number;       // changes after them, only counted (the pin moved too fast)
    droppedUntil: number;       // time of the last of them, in µs
}

// What the pins did since the last message (twin.scope.data); times in µs
// since the twin started.
export interface ScopeData {
    now:        number;         // time of the read
    levels:     number[];       // the level of every pin now
    millivolts: number[];       // the voltage of every pin now
    changes:    ScopeChange[];  // the pins that changed
}

// The I2C transfers (twin.i2c.data).
export interface I2cData {
    records: I2cRecord[];
}

// A control a model declares (the page builds the view of the part from them).
export interface PartControl {
    name:   string;             // the field of the state it shows / sets
    type:   "slider" | "knob" | "switch" | "select" | "button" | "value" | "led";
    label?: string;             // shown with it; the name when missing, none when ""
    min?:   number;             // slider, knob
    max?:   number;
    step?:  number;
    unit?:  string;             // after the value
    items?: string[];           // select
}

// A part on the board (twin.parts).
export interface PartInfo {
    id:       string;
    label:    string;
    model:    string;           // "bme280", or a folder of the project
    controls: PartControl[];    // [] : none
    view:     string | null;    // URL of a view of its own (a project's view.js); null: none
    pins:     number[];         // the pins it is on
}

// The state a part published (twin.part.state).
export type PartState = Record<string, unknown>;

// What a part published (twin.part.state).
export interface PartStateMessage {
    id:    string;
    state: PartState;
}

// A line of a part (twin.log); id null when the part is not known.
export interface LogLine {
    id:   string | null;
    text: string;
}

/**
 * Sends an action to a part ("set" with { name: value } for a control).
 */
export function sendPartAction(id: string, action: string, args: unknown) {
    Application.fireGlobal(MSG.partAction, { id, action, args });
}

/**
 * Asks the twin to force 'pin' to 'level' (0 or 1), or to let it go (-1).
 */
export function forcePin(pin: number, level: number) {
    Application.fireGlobal(MSG.forcePin, { pin, level });
}

/**
 * Sends bytes to the program on UART 'uart' (dropped by the twin if the
 * program does not have it open).
 */
export function sendToUart(uart: number, bytes: Uint8Array) {
    Application.fireGlobal(MSG.uartSend, { uart, bytes: Array.from(bytes) });
}

/**
 * Chooses the port of the TCP link of UART or USB function 'index' and
 * turns its listening on or off (the twin keeps the choice).
 */
export function setTcpLink(kind: TcpKind, index: number, port: number, listen: boolean) {
    Application.fireGlobal(MSG.tcpListen, { kind, index, port, listen });
}
