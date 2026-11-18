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
//   board.pin(n)            level, mode, voltage (volts, read and set), pwm, drive(0 | 1 | null),
//                           pull("up" | "down" | null), onChange(level => ...)
//   board.i2c({ sda, scl }) attach(address, { onTransfer(written, readLength) }), detach(address)
//   board.spi({ port })     attach(cs, { onTransaction(segments, device) }), detach(cs)
//   board.rmt(pin)          onBytes(bytes => ...), onPulses(pulses => ...)
//   board.screen(w, h)      the display built into the board: onFlush(area => ...),
//                           pixels(x, y, width, height)
//   board.now()             ms since the twin started
//   board.restart()         restarts the device (its reset button)
//
// Threads: everything here runs on the twin's thread, one call at a time.
// onTransfer and onTransaction are synchronous: the program waits for their
// answer, so they must be short and must never wait for the program (a
// promise is refused).
import * as twinboard from "qjsx:twinboard";

// The pulls of pin.pull(), as twinboard.pinPull numbers them.
const PULLS = { none: 0, up: 1, down: 2 };

/**
 * A 7-bit I2C address from the config of a part: a number, or a text
 * "0x76" (JSON has no hexadecimal). Throws if it is not one.
 */
export function parseAddress(value) {
    // a text is read as a number ("0x76" gives 118), then checked
    const address = typeof value === "string" ? Number(value) : value;
    const isBadAddress = !Number.isInteger(address) || address < 0 || address > 127;
    if (isBadAddress) {
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
    const isBadPin = !Number.isInteger(value) || value < 0 || value >= count;
    if (isBadPin) {
        throw new Error(`"${name}" must be a pin of the board (0 to ${count - 1}), not ${JSON.stringify(value)}`);
    }

    return value;
}

/**
 * An SPI controller from the config of a part: one of the "ports" of the
 * "spi" section of the board description. Throws a TypeError if it is not
 * an integer, a RangeError if the board has no such controller.
 */
export function parseSpiPort(value) {
    // an integer
    if (!Number.isInteger(value)) {
        throw new TypeError(`"port" must be the number of an SPI controller, not ${JSON.stringify(value)}`);
    }

    // a controller the program may open
    const ports = JSON.parse(twinboard.board()).spi?.ports ?? [];
    if (!ports.includes(value)) {
        const known = ports.length > 0 ? ports.join(", ") : "none";
        throw new RangeError(`"port" ${value} is not an SPI controller of the board (${known})`);
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
        this.id = config.id;                        // unique among the parts
        this.label = config.label ?? config.id;     // shown on the page
        this.config = config;                       // its entry of the wiring
        this.board = board;                         // the Board
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
        // texts as they are, other values as JSON, separated by a space
        const text = values.map(value => (typeof value === "string" ? value : JSON.stringify(value))).join(" ");
        this.board._logged(this, text);
    }
}

// ### Pin #####################################################################

class Pin {
    /**
     * The pin 'number' of the board, with no listener yet.
     */
    constructor(number) {
        this.number = number;           // its number on the board
        this._listeners = [];           // functions of onChange(), called by Board._dispatch
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
     * The voltage on the line now, in volts: what the program's DAC drives
     * (device/dac), else what a component set, else its level times the
     * supply of the board.
     */
    get voltage() {
        return twinboard.pins()[this.number].voltage;
    }

    /**
     * The PWM output of the program on the line (device/pwm): { frequency
     * (Hz), duty (0 to 1, the share of a period it is active), invert (true:
     * active low) }, or null when none drives it.
     */
    get pwm() {
        return twinboard.pins()[this.number].pwm;
    }

    /**
     * Sets the voltage on the line in volts, as a potentiometer does (an
     * analog input reads it; kept within 0 and the supply of the board; a
     * DAC of the program on the pin wins over it); null: not any more.
     */
    set voltage(volts) {
        // !(volts >= 0) also refuses NaN
        const isBadVoltage = volts !== null && (typeof volts !== "number" || !(volts >= 0));
        if (isBadVoltage) {
            throw new TypeError("pin.voltage: volts (0 or more), or null");
        }

        // to the twin in millivolts; -1: not any more
        twinboard.pinVoltage(this.number, volts === null ? -1 : Math.round(volts * 1000));
    }

    /**
     * Drives the line to 0 or 1 (a button pressed); null lets it go.
     */
    drive(level) {
        const isBadLevel = level !== null && level !== 0 && level !== 1;
        if (isBadLevel) {
            throw new TypeError("pin.drive: 0, 1 or null");
        }

        // -1 lets the line go
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
        // kept, and the bridge told to report the changes of this pin
        this._listeners.push(listener);
        twinboard.pinWatch(this.number, true);
    }
}

// ### I2C #####################################################################

class I2cBus {
    /**
     * The bus of the pins 'sda' / 'scl', nothing attached yet ('_board' is
     * set by Board.i2c()).
     */
    constructor(sda, scl) {
        this.sda = sda;               // pin number
        this.scl = scl;               // pin number
        this._handlers = new Map();   // address -> { onTransfer }
        this._parts = new Map();      // address -> id of the part that attached it
    }

    /**
     * Answers the transfers at 'address' with handler.onTransfer(written,
     * readLength): a Uint8Array (or an array) of readLength bytes, or null
     * for a NACK. Throws if a part already answers there.
     */
    attach(address, handler) {
        // a free address, a handler with onTransfer()
        const checked = parseAddress(address);
        if (this._handlers.has(checked)) {
            const owner = this._parts.get(checked);
            throw new Error(`I2C address 0x${checked.toString(16)} is already taken by part "${owner}"`);
        }

        if (typeof handler?.onTransfer !== "function") {
            throw new TypeError("i2c.attach: the handler needs onTransfer(written, readLength)");
        }

        // kept with the part being built, noted on both pins, then the twin
        // answers at that address
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

// ### SPI #####################################################################

// The kinds of segment that read: the model gives their bytes.
const READING_KINDS = ["read", "transfer"];

class SpiBus {
    /**
     * The SPI controller 'port', no component on it yet ('_board' is set by
     * Board.spi()).
     */
    constructor(port) {
        this.port = port;             // controller number
        this._handlers = new Map();   // chip select pin -> { onTransaction }
        this._parts = new Map();      // chip select pin -> id of the part that attached it
    }

    /**
     * Answers the transactions on chip select pin 'cs' (a device whose chip
     * select the hardware drives on that pin, or a device without chip
     * select while the program holds that pin low with device/gpio) with
     * handler.onTransaction(segments, device):
     *   segments  [{ kind: "write" | "read" | "transfer" | "delay", size,
     *             dc (the level of the DC pin: 0 command, 1 data), bytes
     *             (write, transfer: a Uint8Array), delayUs (delay) }]
     *   device    { port, cs, frequency (Hz), mode (0..3), bitOrder ("msb" | "lsb") }
     * It returns the bytes of the read and transfer segments, in their
     * order: an array with one entry per such segment (a Uint8Array, an
     * array of numbers, or null), or null for none. Missing bytes read 0xFF,
     * bytes beyond the size of their segment are dropped. Throws if a part
     * already sits on that chip select.
     */
    attach(cs, handler) {
        // a free chip select, a handler with onTransaction()
        const pin = parsePin(cs, "cs");
        if (this._handlers.has(pin)) {
            const owner = this._parts.get(pin);
            throw new Error(`SPI chip select ${pin} of controller ${this.port} is already taken by part "${owner}"`);
        }

        if (typeof handler?.onTransaction !== "function") {
            throw new TypeError("spi.attach: the handler needs onTransaction(segments, device)");
        }

        // kept with the part being built, noted on its pin, then the twin
        // gives it the transactions on that chip select
        const part = this._board._currentPart ?? "?";
        this._handlers.set(pin, handler);
        this._parts.set(pin, part);
        this._board._noteUse(pin, part);
        twinboard.spiAttach(this.port, pin, true);
    }

    /**
     * Nothing sits on chip select pin 'cs' any more.
     */
    detach(cs) {
        const pin = parsePin(cs, "cs");
        this._handlers.delete(pin);
        this._parts.delete(pin);
        twinboard.spiAttach(this.port, pin, false);
    }
}

/**
 * The bytes a model gives for the reading segments of a transaction, one
 * after the other, each padded with 0xFF to the size of its segment (all
 * 0xFF for 'given' null). Throws a TypeError when 'given' is not what
 * onTransaction must return.
 */
function readBytes(reading, given) {
    // all 0xFF to start with
    const total = reading.reduce((sum, segment) => sum + segment.size, 0);
    const bytes = new Uint8Array(total).fill(0xff);
    const isNothing = given === null || given === undefined;
    if (isNothing) {
        return bytes;
    }

    // an array, one entry per reading segment
    if (!Array.isArray(given)) {
        throw new TypeError("onTransaction must return an array (one entry per read or transfer segment) or null, not a promise");
    }

    // each entry at the place of its segment, cut to its size
    let offset = 0;

    reading.forEach((segment, index) => {
        const entry = given[index];
        const isBytes = entry instanceof Uint8Array || Array.isArray(entry);
        const isEmpty = entry === null || entry === undefined;
        if (!isBytes && !isEmpty) {
            throw new TypeError(`onTransaction: entry ${index} must be bytes (a Uint8Array or an array of numbers) or null`);
        }

        if (isBytes) {
            bytes.set(entry.slice(0, segment.size), offset);
        }

        offset += segment.size;
    });

    return bytes;
}

// ### The display built into the board ########################################

class BuiltinScreen {
    /**
     * The display of 'width' x 'height' pixels (its image is made by
     * Board.screen()), no listener yet.
     */
    constructor(width, height) {
        this.width = width;             // pixels, as the panel scans it
        this.height = height;
        this._listeners = [];           // functions of onFlush(), called by Board._dispatch
    }

    /**
     * Calls 'listener({ x, y, width, height, rotation })' each time the
     * program makes an area of the image visible (pixels of the image; the
     * rotation of its drawing, 0, 90, 180, 270 clockwise).
     */
    onFlush(listener) {
        this._listeners.push(listener);
    }

    /**
     * The pixels of an area of the image, cut to it: a Uint8Array, 16 bits a
     * pixel (5 red, 6 green, 5 blue), high byte first, row by row.
     */
    pixels(x, y, width, height) {
        return twinboard.screenPixels(x, y, width, height);
    }
}

// ### RMT #####################################################################

class RmtLine {
    /**
     * What device/rmt sends on 'pin', with no listener yet.
     */
    constructor(pin) {
        this.pin = pin;                 // pin number
        this._bytes = [];               // listeners of onBytes()
        this._pulses = [];              // listeners of onPulses()
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
    /**
     * The board, no part on it yet ('hooks': see above).
     */
    constructor(hooks) {
        this._hooks = hooks;            // published, logged, failed
        this._start = Date.now();       // ms, Date.now() when the twin started
        this._pins = new Map();         // number -> Pin
        this._pinOwners = new Map();    // number -> id of the part that took it
        this._pinUsers = new Map();     // number -> Set of ids of the parts on it (taken, or a shared bus)
        this._buses = new Map();        // "sda/scl" -> I2cBus
        this._spiBuses = new Map();     // port -> SpiBus
        this._rmt = new Map();          // pin -> RmtLine
        this._screen = null;            // the BuiltinScreen; null: none
        this._currentPart = undefined;  // id of the part being built (pins taken)
    }

    /**
     * Milliseconds since the twin started.
     */
    now() {
        return Date.now() - this._start;
    }

    /**
     * Restarts the device, as its reset button (the EN pin of an ESP32): a
     * program loaded in RAM is dropped, the installed one runs again. The
     * parts stay as they are.
     */
    restart() {
        twinboard.reset();
    }

    /**
     * The supply of the board in volts: the level 1 of a pin, the most an
     * analog input reads ("adc.vref" of the description, 3.3 by default).
     */
    get supply() {
        // read once from the description of the board
        if (this._supply === undefined) {
            this._supply = JSON.parse(twinboard.board()).adc?.vref ?? 3.3;
        }

        return this._supply;
    }

    /**
     * A pin of the board. Throws if another part already took it.
     */
    pin(number) {
        // taken by another part: refused
        const checked = parsePin(number);
        const owner = this._pinOwners.get(checked);
        const part = this._currentPart;
        const isTakenByAnother = owner !== undefined && part !== undefined && owner !== part;
        if (isTakenByAnother) {
            throw new Error(`pin ${checked} is already used by part "${owner}"`);
        }

        // asked while a part is built: the pin is that part's
        if (part !== undefined) {
            this._pinOwners.set(checked, part);
            this._noteUse(checked, part);
        }

        // one Pin per number, made at the first request
        if (!this._pins.has(checked)) {
            this._pins.set(checked, new Pin(checked));
        }

        return this._pins.get(checked);
    }

    /**
     * The I2C bus of the pins 'sda' / 'scl' (shared by the parts on it).
     */
    i2c({ sda, scl } = {}) {
        // two different pins
        const sdaPin = parsePin(sda, "sda");
        const sclPin = parsePin(scl, "scl");
        if (sdaPin === sclPin) {
            throw new Error('"sda" and "scl" must be two pins');
        }

        // one bus per pair of pins, made at the first request
        const key = `${sdaPin}/${sclPin}`;
        if (!this._buses.has(key)) {
            const bus = new I2cBus(sdaPin, sclPin);
            bus._board = this;
            this._buses.set(key, bus);
        }

        return this._buses.get(key);
    }

    /**
     * The SPI controller 'port' (one of the "ports" of the board's "spi"),
     * shared by the parts on it.
     */
    spi({ port } = {}) {
        // one bus per controller, made at the first request
        const checked = parseSpiPort(port);
        if (!this._spiBuses.has(checked)) {
            const bus = new SpiBus(checked);
            bus._board = this;
            this._spiBuses.set(checked, bus);
        }

        return this._spiBuses.get(checked);
    }

    /**
     * What device/rmt sends on a pin.
     */
    rmt(pin) {
        // asked while a part is built: noted on the pin (not exclusive)
        const checked = parsePin(pin);
        if (this._currentPart !== undefined) {
            this._noteUse(checked, this._currentPart);
        }

        // one line per pin, made at the first request
        if (!this._rmt.has(checked)) {
            this._rmt.set(checked, new RmtLine(checked));
        }

        return this._rmt.get(checked);
    }

    /**
     * The display built into the board (its image in memory, 'width' x
     * 'height' pixels, as the panel scans it), made at the first request:
     * sys/draw draws into it as on the board. Throws if it was made with
     * another size.
     */
    screen(width, height) {
        // one display: made once, the same size asked again
        if (this._screen === null) {
            this._screen = new BuiltinScreen(width, height);
        }

        const isSameSize = this._screen.width === width && this._screen.height === height;
        if (!isSameSize || !twinboard.screenStart(width, height)) {
            throw new Error(`the board has a display of ${this._screen.width} x ${this._screen.height} already`);
        }

        return this._screen;
    }

    /**
     * The pins each part is on (taken, or its bus): id -> [numbers].
     */
    pinsOfParts() {
        const result = {};

        for (const [pin, parts] of this._pinUsers) {
            for (const part of parts) {
                // '??=' makes the list of a part at its first pin
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

    /**
     * Part.publish(): to the twin's hook.
     */
    _published(part, state) {
        this._hooks.published(part, state);
    }

    /**
     * Part.log(): to the twin's hook.
     */
    _logged(part, text) {
        this._hooks.logged(part, text);
    }

    /**
     * An event of the bridge (twinboard.Bridge).
     */
    _dispatch(event) {
        switch (event.type) {
            case "pin": {
                // a level changed: the listeners of that pin
                const listeners = this._pins.get(event.pin)?._listeners ?? [];
                for (const listener of listeners) {
                    this._call(() => listener(event.level));
                }

                break;
            }
            case "rmtBytes": {
                // bytes sent: the byte listeners of that pin
                const listeners = this._rmt.get(event.pin)?._bytes ?? [];
                for (const listener of listeners) {
                    this._call(() => listener(event.bytes));
                }

                break;
            }
            case "rmtPulses": {
                // pulses sent: the pulse listeners of that pin
                const listeners = this._rmt.get(event.pin)?._pulses ?? [];
                for (const listener of listeners) {
                    this._call(() => listener(event.pulses));
                }

                break;
            }
            case "i2c": {
                // a transfer: answered at once
                this._answerTransfer(event);
                break;
            }
            case "spi": {
                // a transaction: answered at once
                this._answerTransaction(event);
                break;
            }
            case "screen": {
                // an area of the built-in display made visible: its listeners
                const listeners = this._screen?._listeners ?? [];
                const area = { x: event.x, y: event.y, width: event.width, height: event.height, rotation: event.rotation };
                for (const listener of listeners) {
                    this._call(() => listener(area));
                }

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
        // nobody at that address: a NACK
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

        // its answer to the program: a NACK, bytes, or a failed transfer
        const isNack = answer === null || answer === undefined;
        if (isNack) {
            twinboard.i2cAnswer(event.request, "nack");
        } else if (answer instanceof Uint8Array || Array.isArray(answer)) {
            const bytes = answer instanceof Uint8Array ? answer : new Uint8Array(answer);
            twinboard.i2cAnswer(event.request, "ok", bytes);
        } else {
            const part = bus._parts.get(event.address);
            this._hooks.failed(part, new TypeError("onTransfer must return bytes or null (not a promise)"));
            twinboard.i2cAnswer(event.request, "failed");
        }
    }

    /**
     * An SPI transaction of the program: asks the handler on its chip
     * select, then gives the bytes read back at once (the program waits for
     * them). A handler that throws or answers wrong: its error goes to the
     * console of the page and the program reads 0xFF (an SPI bus has no way
     * to fail).
     */
    _answerTransaction(event) {
        // the segments that read, and the handler on that chip select
        const reading = event.segments.filter(segment => READING_KINDS.includes(segment.kind));
        const bus = this._spiBuses.get(event.port);
        const handler = bus?._handlers.get(event.cs);
        if (handler === undefined) {
            twinboard.spiAnswer(event.request, readBytes(reading, null));
            return;
        }

        // the handler's answer, as bytes; its error: 0xFF read
        const device = { port: event.port, cs: event.cs, frequency: event.frequency, mode: event.mode, bitOrder: event.bitOrder };
        let bytes;
        try {
            bytes = readBytes(reading, handler.onTransaction(event.segments, device));
        } catch (error) {
            this._hooks.failed(bus._parts.get(event.cs), error);
            bytes = readBytes(reading, null);
        }

        twinboard.spiAnswer(event.request, bytes);
    }
}

// ### Registers ###############################################################

// The registers of a chip on an I2C bus, as most sensors have them: the
// first byte written sets the register pointer; a read gives the registers
// from the pointer on, the pointer moving at each byte. Writes either move
// the pointer too ("increment", the default), or come as register / value
// pairs ("pairs", as the BME280). Usable as the handler of i2c.attach().
export class Registers {
    /**
     * 'size' registers at 0, the pointer at 0; 'writes': "increment" or
     * "pairs" (see above).
     */
    constructor(size = 256, { writes = "increment" } = {}) {
        this.bytes = new Uint8Array(size);   // the registers
        this.pointer = 0;                    // register of the next read or write
        this._writes = writes;               // "increment" or "pairs"
        this._writeHandlers = new Map();     // register -> [fn(value, register)]
        this._readHandlers = [];             // [fn(start, length)]
    }

    /**
     * Sets registers from 'register' on (a number or an array of numbers),
     * as the chip itself would (no write handler called).
     */
    set(register, values) {
        // one value is a list of one; the registers wrap around at the end
        const isList = Array.isArray(values) || values instanceof Uint8Array;
        const list = isList ? values : [values];
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
            this.pointer = written[0] % this.bytes.length;
            if (this._writes === "pairs") {
                this._writePairs(written);
            } else {
                for (let index = 1; index < written.length; index++) {
                    this._write(this.pointer, written[index]);
                    this.pointer = (this.pointer + 1) % this.bytes.length;
                }
            }
        }

        // nothing to read: no read handler called
        if (readLength === 0) {
            return new Uint8Array(0);
        }

        // the read handlers refresh the registers first
        for (const handler of this._readHandlers) {
            handler(this.pointer, readLength);
        }

        // the read, from the pointer on
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
     * One register written by the program, then its handlers. The register
     * wraps around at the end, as for reads (a part with fewer than 256
     * registers in "pairs" mode).
     */
    _write(register, value) {
        const wrapped = register % this.bytes.length;
        this.bytes[wrapped] = value;

        for (const handler of this._writeHandlers.get(wrapped) ?? []) {
            handler(value, wrapped);
        }
    }
}
