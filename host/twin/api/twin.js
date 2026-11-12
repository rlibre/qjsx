/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// qjsx:twin, the public API of the models of the parts of a digital twin
// (doc/architecture.md 6.6). A model plays a component wired on the board
// (a button, an LED, a BME280...): it runs in the twin's JS runtime, never in
// the program's, and sees the board through 'board':
//
//   import { Part, Registers } from "qjsx:twin";
//
//   export default class Bme280 extends Part {
//       static controls = [{ name: "temperature", type: "slider", min: -40, max: 85 }];
//       constructor(config, board) {
//           super(config, board);
//           board.i2c(config.i2c).attach(0x76, new Registers(256));
//       }
//       onAction(action, args) { ... }       // from the page
//   }
//
//   board.pin(n)            level, mode, voltage (volts, read and set), drive(0 | 1 | null),
//                           pull("up" | "down" | null), onChange(level => ...)| 1 | null), pull("up" | "down" | null),
//                           onChange(level => ...)
//   board.i2c({ sda, scl }) attach(address, { onTransfer(written, readLength) }), detach(address)
//   board.rmt(pin)          onBytes(bytes => ...), onPulses(pulses => ...)
//   board.now()             ms since the twin started
//
// Threads: everything here runs on the twin's thread, one call at a time.
// onTransfer is synchronous: the program waits for its answer, so it must
// be short and must never wait for the program (a promise is refused).
import * as twinboard from "qjsx:twinboard";

// The pulls of pin.pull(), as twinboard.pinPull numbers them.
const PULLS = { none: 0, up: 1, down: 2 };

/**
 * A 7-bit I2C address from the config of a part: a number, or a text
 * "0x76" (JSON has no hexadecimal). Throws if it is not one.
 */
export function parseAddress(value) {
    const address = typeof value === "string" ? Number(value) : value;
    if (!Number.isInteger(address) || address < 0 || address > 127) {
        throw new Error(`"address" must be a 7-bit I2C address ("0x76" or 118), not ${JSON.stringify(value)}`);
    }
    return address;
}

/**
 * A pin number from the config of a part ('name' says which field); throws
 * if it is not a pin of the board.
 */
export function parsePin(value, name = "pin") {
    const count = twinboard.pins().length;
    if (!Number.isInteger(value) || value < 0 || value >= count) {
        throw new Error(`"${name}" must be a pin of the board (0 to ${count - 1}), not ${JSON.stringify(value)}`);
    }
    return value;
}

// ### Part ####################################################################

// The base class of a model. A subclass may declare 'static controls' (the
// page builds its view from them) and override onAction().
export class Part {
    /**
     * 'config': the part's entry of the wiring (id, label, model, and its own
     * fields, which the subclass checks); 'board': the board.
     */
    constructor(config, board) {
        this.id = config.id;
        this.label = config.label ?? config.id;
        this.config = config;
        this.board = board;
    }

    /**
     * Sends the state of the part to the pages (twin.part.state); the last
     * one is also given to a page that connects later.
     */
    publish(state) {
        this.board._published(this, state);
    }

    /**
     * An action of the page (twin.part.action): "set" with { name: value }
     * for a declared control, or what a view of its own sends.
     */
    onAction(action, args) {
    }

    /**
     * A line in the console of the models on the page (this runtime prints
     * nothing: its output would mix with the program's).
     */
    log(...values) {
        const text = values.map(value => (typeof value === "string" ? value : JSON.stringify(value))).join(" ");
        this.board._logged(this, text);
    }
}

// ### Pin #####################################################################

class Pin {
    constructor(number) {
        this.number = number;
        this._listeners = [];
    }

    /**
     * The level on the line now (0 or 1).
     */
    get level() {
        return twinboard.pins()[this.number].level;
    }

    /**
     * What the program configured: "input", "output", "openDrain", or null.
     */
    get mode() {
        const state = twinboard.pins()[this.number];
        return state.configured ? state.mode : null;
    }

    /**
     * The voltage on the line now, in volts: what a component set, else its
     * level times the supply of the board.
     */
    get voltage() {
        return twinboard.pins()[this.number].voltage;
    }

    /**
     * Sets the voltage on the line in volts, as a potentiometer does (an
     * analog input reads it; kept within 0 and the supply of the board);
     * null: not any more.
     */
    set voltage(volts) {
        if (volts !== null && (typeof volts !== "number" || !(volts >= 0))) {
            throw new TypeError("pin.voltage: volts (0 or more), or null");
        }
        twinboard.pinVoltage(this.number, volts === null ? -1 : Math.round(volts * 1000));
    }

    /**
     * Drives the line to 0 or 1 (a button pressed); null lets it go.
     */
    drive(level) {
        if (level !== null && level !== 0 && level !== 1) {
            throw new TypeError("pin.drive: 0, 1 or null");
        }
        twinboard.pinDrive(this.number, level === null ? -1 : level);
    }

    /**
     * Pulls the line: "up", "down", or null (the pull-up of an active low
     * button, on the board).
     */
    pull(direction) {
        const pull = PULLS[direction ?? "none"];
        if (pull === undefined) {
            throw new TypeError('pin.pull: "up", "down" or null');
        }
        twinboard.pinPull(this.number, pull);
    }

    /**
     * Calls 'listener(level)' after each change of the level (a little after:
     * the change comes through the bridge).
     */
    onChange(listener) {
        this._listeners.push(listener);
        twinboard.pinWatch(this.number, true);
    }
}

// ### I2C #####################################################################

class I2cBus {
    constructor(sda, scl) {
        this.sda = sda;
        this.scl = scl;
        this._handlers = new Map();   // address -> { onTransfer }
        this._parts = new Map();      // address -> id of the part that attached it
    }

    /**
     * Answers the transfers at 'address' with handler.onTransfer(written,
     * readLength): a Uint8Array (or an array) of readLength bytes, or null
     * for a NACK. Throws if a part already answers there.
     */
    attach(address, handler) {
        const checked = parseAddress(address);
        if (this._handlers.has(checked)) {
            const owner = this._parts.get(checked);
            throw new Error(`I2C address 0x${checked.toString(16)} is already taken by part "${owner}"`);
        }
        if (typeof handler?.onTransfer !== "function") {
            throw new TypeError("i2c.attach: the handler needs onTransfer(written, readLength)");
        }
        const part = this._board._currentPart ?? "?";
        this._handlers.set(checked, handler);
        this._parts.set(checked, part);
        this._board._noteUse(this.sda, part);
        this._board._noteUse(this.scl, part);
        twinboard.i2cAttach(this.sda, this.scl, checked, true);
    }

    /**
     * Nothing answers at 'address' any more.
     */
    detach(address) {
        const checked = parseAddress(address);
        this._handlers.delete(checked);
        this._parts.delete(checked);
        twinboard.i2cAttach(this.sda, this.scl, checked, false);
    }
}

// ### RMT #####################################################################

class RmtLine {
    constructor(pin) {
        this.pin = pin;
        this._bytes = [];
        this._pulses = [];
    }

    /**
     * Calls 'listener(bytes)' with the bytes of each sendBytes on the pin.
     */
    onBytes(listener) {
        this._bytes.push(listener);
    }

    /**
     * Calls 'listener(pulses)' with the pulses of each send on the pin:
     * [ticks, level, ticks, level, ...].
     */
    onPulses(listener) {
        this._pulses.push(listener);
    }
}

// ### Board ###################################################################

// The board as the models see it. Made by the twin (qjsx:twin/parts), not by
// the models: 'hooks' = { published(part, state), logged(part, text),
// failed(id, error) } (id of the part, undefined when unknown).
export class Board {
    constructor(hooks) {
        this._hooks = hooks;
        this._start = Date.now();
        this._pins = new Map();         // number -> Pin
        this._pinOwners = new Map();    // number -> id of the part that took it
        this._pinUsers = new Map();     // number -> Set of ids of the parts on it (taken, or a shared bus)
        this._buses = new Map();        // "sda/scl" -> I2cBus
        this._rmt = new Map();          // pin -> RmtLine
        this._currentPart = undefined;  // id of the part being built (pins taken)
    }

    /**
     * Milliseconds since the twin started.
     */
    now() {
        return Date.now() - this._start;
    }

    /**
     * The supply of the board in volts: the level 1 of a pin, the most an
     * analog input reads ("adc.vref" of the description, 3.3 by default).
     */
    get supply() {
        if (this._supply === undefined) {
            this._supply = JSON.parse(twinboard.board()).adc?.vref ?? 3.3;
        }
        return this._supply;
    }

    /**
     * A pin of the board. Throws if another part already took it.
     */
    pin(number) {
        const checked = parsePin(number);
        const owner = this._pinOwners.get(checked);
        const part = this._currentPart;
        if (owner !== undefined && part !== undefined && owner !== part) {
            throw new Error(`pin ${checked} is already used by part "${owner}"`);
        }
        if (part !== undefined) {
            this._pinOwners.set(checked, part);
            this._noteUse(checked, part);
        }

        if (!this._pins.has(checked)) {
            this._pins.set(checked, new Pin(checked));
        }
        return this._pins.get(checked);
    }

    /**
     * The I2C bus of the pins 'sda' / 'scl' (shared by the parts on it).
     */
    i2c({ sda, scl } = {}) {
        const sdaPin = parsePin(sda, "sda");
        const sclPin = parsePin(scl, "scl");
        if (sdaPin === sclPin) {
            throw new Error('"sda" and "scl" must be two pins');
        }

        const key = `${sdaPin}/${sclPin}`;
        if (!this._buses.has(key)) {
            const bus = new I2cBus(sdaPin, sclPin);
            bus._board = this;
            this._buses.set(key, bus);
        }
        return this._buses.get(key);
    }

    /**
     * What device/rmt sends on a pin.
     */
    rmt(pin) {
        const checked = parsePin(pin);
        if (this._currentPart !== undefined) {
            this._noteUse(checked, this._currentPart);
        }
        if (!this._rmt.has(checked)) {
            this._rmt.set(checked, new RmtLine(checked));
        }
        return this._rmt.get(checked);
    }

    /**
     * The pins each part is on (taken, or its bus): id -> [numbers].
     */
    pinsOfParts() {
        const result = {};
        for (const [pin, parts] of this._pinUsers) {
            for (const part of parts) {
                (result[part] ??= []).push(pin);
            }
        }
        return result;
    }

    // ### for the twin (qjsx:twin/parts) ######################################

    /**
     * Notes that 'part' is on 'pin' (for the page; not exclusive).
     */
    _noteUse(pin, part) {
        if (!this._pinUsers.has(pin)) {
            this._pinUsers.set(pin, new Set());
        }
        this._pinUsers.get(pin).add(part);
    }

    _published(part, state) {
        this._hooks.published(part, state);
    }

    _logged(part, text) {
        this._hooks.logged(part, text);
    }

    /**
     * An event of the bridge (twinboard.Bridge).
     */
    _dispatch(event) {
        switch (event.type) {
        case "pin": {
            for (const listener of this._pins.get(event.pin)?._listeners ?? []) {
                this._call(() => listener(event.level));
            }
            break;
        }
        case "rmtBytes": {
            for (const listener of this._rmt.get(event.pin)?._bytes ?? []) {
                this._call(() => listener(event.bytes));
            }
            break;
        }
        case "rmtPulses": {
            for (const listener of this._rmt.get(event.pin)?._pulses ?? []) {
                this._call(() => listener(event.pulses));
            }
            break;
        }
        case "i2c": {
            this._answerTransfer(event);
            break;
        }
        default: {
            break;
        }
        }
    }

    /**
     * Calls a listener of a model; an exception goes to the console of the
     * models instead of stopping the twin.
     */
    _call(callback) {
        try {
            callback();
        } catch (error) {
            this._hooks.failed(undefined, error);
        }
    }

    /**
     * A transfer of the program: asks the handler at that address, then gives
     * the answer back at once (the program waits for it).
     */
    _answerTransfer(event) {
        const bus = this._buses.get(`${event.sda}/${event.scl}`);
        const handler = bus?._handlers.get(event.address);
        if (handler === undefined) {
            twinboard.i2cAnswer(event.request, "nack");
            return;
        }

        // the handler: bytes, null (NACK); a promise or an exception fails
        let answer;
        try {
            answer = handler.onTransfer(event.written, event.readLength);
        } catch (error) {
            this._hooks.failed(bus._parts.get(event.address), error);
            twinboard.i2cAnswer(event.request, "failed");
            return;
        }

        if (answer === null || answer === undefined) {
            twinboard.i2cAnswer(event.request, "nack");
        } else if (answer instanceof Uint8Array || Array.isArray(answer)) {
            twinboard.i2cAnswer(event.request, "ok", answer instanceof Uint8Array ? answer : new Uint8Array(answer));
        } else {
            const part = bus._parts.get(event.address);
            this._hooks.failed(part, new TypeError("onTransfer must return bytes or null (not a promise)"));
            twinboard.i2cAnswer(event.request, "failed");
        }
    }
}

// ### Registers ###############################################################

// The registers of a chip on an I2C bus, as most sensors have them: the
// first byte written sets the register pointer; a read gives the registers
// from the pointer on, the pointer moving at each byte. Writes either move
// the pointer too ("increment", the default), or come as register / value
// pairs ("pairs", as the BME280). Usable as the handler of i2c.attach().
export class Registers {
    constructor(size = 256, { writes = "increment" } = {}) {
        this.bytes = new Uint8Array(size);
        this.pointer = 0;
        this._writes = writes;
        this._writeHandlers = new Map();   // register -> [fn(value, register)]
        this._readHandlers = [];           // [fn(start, length)]
    }

    /**
     * Sets registers from 'register' on (a number or an array of numbers),
     * as the chip itself would (no write handler called).
     */
    set(register, values) {
        const list = Array.isArray(values) || values instanceof Uint8Array ? values : [values];
        list.forEach((value, index) => {
            this.bytes[(register + index) % this.bytes.length] = value & 0xff;
        });
    }

    /**
     * The value of a register.
     */
    get(register) {
        return this.bytes[register % this.bytes.length];
    }

    /**
     * Calls 'handler(value, register)' after the program writes 'register'.
     */
    onWrite(register, handler) {
        if (!this._writeHandlers.has(register)) {
            this._writeHandlers.set(register, []);
        }
        this._writeHandlers.get(register).push(handler);
    }

    /**
     * Calls 'handler(start, length)' before each read of the program (to
     * refresh what it is about to read).
     */
    onRead(handler) {
        this._readHandlers.push(handler);
    }

    /**
     * The transfer of the program (i2c.attach handler).
     */
    onTransfer(written, readLength) {
        // the writes: the pointer, then the values
        if (written.length > 0) {
            this.pointer = written[0];
            if (this._writes === "pairs") {
                this._writePairs(written);
            } else {
                for (let index = 1; index < written.length; index++) {
                    this._write(this.pointer, written[index]);
                    this.pointer = (this.pointer + 1) % this.bytes.length;
                }
            }
        }

        // the read, from the pointer on
        if (readLength === 0) {
            return new Uint8Array(0);
        }
        for (const handler of this._readHandlers) {
            handler(this.pointer, readLength);
        }
        const answer = new Uint8Array(readLength);
        for (let index = 0; index < readLength; index++) {
            answer[index] = this.bytes[this.pointer];
            this.pointer = (this.pointer + 1) % this.bytes.length;
        }
        return answer;
    }

    /**
     * Register / value pairs: [register, value, register, value, ...].
     */
    _writePairs(written) {
        for (let index = 0; index + 1 < written.length; index += 2) {
            this._write(written[index], written[index + 1]);
        }
    }

    /**
     * One register written by the program, then its handlers.
     */
    _write(register, value) {
        this.bytes[register] = value;
        for (const handler of this._writeHandlers.get(register) ?? []) {
            handler(value, register);
        }
    }
}
