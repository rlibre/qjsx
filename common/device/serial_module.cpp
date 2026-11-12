/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "device/serial": serial ports, through the driver chosen when the
// process starts (hal_serial.h, setSerialDriver): the ports of the PC, the
// UARTs of a board, or those of the digital twin of a board. One JS class,
// Serial, for all of them.
//
//   import { list, Serial } from "device/serial";
//   list()                                    PC: ["COM3", ...]; board: [1, 2] (free UARTs)
//   const port = new Serial("COM3", { baudRate: 115200 });                  // PC
//   const uart = new Serial(2, { baudRate: 115200, tx: 38, rx: 37 });       // board
//   port.onData(bytes => ...);                // Uint8Array, one block per burst received
//   port.onClose(() => ...);
//   port.write("text" or bytes);              // synchronous
//   port.port();                              // "COM3", or 2
//   port.close();
//
// Options: baudRate (115200), dataBits (8), parity ("none", "even", "odd"),
// stopBits (1); on the PC flowControl ("none", "rts-cts"); on a board the pins
// tx, rx (at least one), and rts with cts for the flow control. A pin not
// given is not wired.
//
// Threads: a reader thread per open port calls the driver's read (it returns
// within 100 ms) and posts the received bytes to the event loop; write() runs
// on the JS thread. The state shared by the two sides is reference counted:
// the last one to let it go closes the port, so that close(), a read error
// and the end of the run can come in any order.
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "jsbind.h"
#include "device_modules.h"
#include "hal_serial.h"
#include "loop.h"
#include "memory.h"
#include "permissions.h"
#include "platform.h"

namespace {

enum MessageKind {
    MESSAGE_DATA = 1,                       // bytes received: data / size
    MESSAGE_CLOSED,                         // the reader thread has stopped
};

const int DEFAULT_BAUD_RATE = 115200;       // bits per second
const size_t READ_BLOCK = 1024;             // bytes read at most at once

const hal::serial::Driver *driver = nullptr;   // set before any runtime exists (setSerialDriver)

JS_ENUM(Parity, none, even, odd);

// Options of 'new Serial(port, options)'.
struct SerialOptions {
    js::Range<int, 300, 4000000> baudRate = DEFAULT_BAUD_RATE;   // bits per second
    js::Range<int, 5, 8> dataBits = 8;                           // bits of a character
    Parity parity = Parity::none;                                // parity bit
    js::Range<int, 1, 2> stopBits = 1;                           // stop bits
    js::Optional<js::Str> flowControl;                           // PC: "none" or "rts-cts"
    js::Optional<js::Range<int, 0, 255>> tx;                     // board: pins; absent: not wired
    js::Optional<js::Range<int, 0, 255>> rx;
    js::Optional<js::Range<int, 0, 255>> rts;
    js::Optional<js::Range<int, 0, 255>> cts;
};
JS_STRUCT(SerialOptions, baudRate, dataBits, parity, stopBits, flowControl, tx, rx, rts, cts);

// Shared by the JS thread and the reader thread. Allocated by Serial::start(),
// freed by the last of its two owners (releasePortState).
struct PortState {
    hal::serial::Port *port;                // owned: closed by the last owner
    int                closing;             // atomic; set by close(): the reader thread stops
    int                references;          // atomic; 2 owners: the Serial object and the reader thread
    rt::Mailbox       *mailbox;             // retained until the state is freed
    rt::SourceId       source;              // message source of the Serial object
};

/**
 * One owner of the state lets it go; the last one closes the port and frees
 * the state.
 */
void releasePortState(PortState *state) {
    if (platform::atomicDecrement(&state->references) > 0) {
        return;
    }

    driver->close(state->port);
    state->mailbox->release();
    memory::release(state);
}

/**
 * Reader thread of a port: posts the received bytes, then MESSAGE_CLOSED when
 * it stops (close() asked, or a read error). Owns one share of 'argument',
 * the PortState.
 */
void readLoop(void *argument) {
    PortState *state = (PortState *)argument;
    uint8_t *block = nullptr;

    for (;;) {
        // stop when the JS thread sets 'closing' (seen within 100 ms, the
        // longest wait of a read)
        if (__atomic_load_n(&state->closing, __ATOMIC_ACQUIRE)) {
            break;
        }

        // the bytes are read straight into the block posted to the loop (no
        // buffer on the stack, no copy); the block is reused while reads time
        // out empty
        if (block == nullptr) {
            block = (uint8_t *)memory::allocate(READ_BLOCK);
        }

        // read what is there; a read error closes the port
        int received = driver->read(state->port, block, READ_BLOCK);
        if (received < 0) {
            break;
        }

        // bytes: post the block shrunk to their size
        if (received > 0) {
            uint8_t *data = (uint8_t *)memory::reallocate(block, (size_t)received);
            state->mailbox->post(state->source, MESSAGE_DATA, data, received, 0);
            block = nullptr;   // now owned by the loop
        }
    }

    memory::release(block);

    // tell the JS thread, then let the state go (the last owner closes the
    // port: the JS side may still be writing)
    state->mailbox->post(state->source, MESSAGE_CLOSED, nullptr, 0, 0);
    releasePortState(state);
}

/**
 * True if the run may open the port 'name' of the PC (nullptr: list the
 * ports; rt::Permissions::mayUseSerial); else throws EACCES saying how to
 * grant it, and returns false. The UARTs of a board need no permission.
 */
bool serialAllowed(const char *name) {
    // the tool's own commands and the devices have no permissions:
    // everything allowed
    const rt::Permissions *permissions = rt::permissions();
    if (permissions == nullptr || permissions->mayUseSerial(name)) {
        return true;
    }

    // refused
    char what[300];
    const char *hint = permissions->hint();
    const char *target = name != nullptr ? name : "the list of the ports";
    snprintf(what, sizeof(what), "serial access to \"%s\" is not allowed%s%s", target, hint[0] != 0 ? ": " : "",
             hint);
    js::failSystem(EACCES, what);
    return false;
}

/**
 * The flow control of the options of a PC port into *flowControl: absent or
 * "none" is off, "rts-cts" on. Throws a TypeError for anything else and
 * returns false.
 */
bool readFlowControl(const js::Optional<js::Str> *option, bool *flowControl) {
    *flowControl = false;
    if (!option->has()) {
        return true;
    }

    const char *text = option->value().data();
    if (strcmp(text, "none") == 0) {
        return true;
    }

    if (strcmp(text, "rts-cts") == 0) {
        *flowControl = true;
        return true;
    }

    js::fail<js::TypeError>("Serial: flowControl must be \"none\" or \"rts-cts\"");
    return false;
}

/**
 * The pins of the options of a board UART into *settings (NO_PIN when not
 * given): tx or rx at least, rts and cts together (they turn the flow control
 * on). Throws a TypeError and returns false otherwise.
 */
bool readPins(const SerialOptions *options, hal::serial::Settings *settings) {
    // a pin given, or not wired
    settings->tx = options->tx.has() ? (int)options->tx.value() : hal::serial::NO_PIN;
    settings->rx = options->rx.has() ? (int)options->rx.value() : hal::serial::NO_PIN;
    settings->rts = options->rts.has() ? (int)options->rts.value() : hal::serial::NO_PIN;
    settings->cts = options->cts.has() ? (int)options->cts.value() : hal::serial::NO_PIN;

    // a UART with no data line is of no use
    if (!options->tx.has() && !options->rx.has()) {
        js::fail<js::TypeError>("Serial: give the tx pin, the rx pin, or both");
        return false;
    }

    // the flow control needs both lines
    if (options->rts.has() != options->cts.has()) {
        js::fail<js::TypeError>("Serial: rts and cts go together (flow control)");
        return false;
    }

    // on a board, the flow control is the pins
    if (options->flowControl.has()) {
        js::fail<js::TypeError>("Serial: on a board, give the rts and cts pins instead of flowControl");
        return false;
    }

    settings->flowControl = options->rts.has();
    return true;
}

/**
 * The options of 'new Serial(port, options)' into *settings, as the driver
 * wants them (pins for a board, flowControl for the PC). Throws a TypeError
 * and returns false on an option that does not fit the driver.
 */
bool readSettings(const SerialOptions *options, hal::serial::Settings *settings) {
    // what every driver has
    settings->baudRate = (int)options->baudRate;
    settings->dataBits = (int)options->dataBits;
    settings->parity = options->parity == Parity::even  ? hal::serial::Parity::even
                     : options->parity == Parity::odd   ? hal::serial::Parity::odd
                                                        : hal::serial::Parity::none;
    settings->stopBits = (int)options->stopBits;

    // a board: the pins
    if (!driver->namedPorts) {
        return readPins(options, settings);
    }

    // the PC: no pins, the flow control by name
    bool pinGiven = options->tx.has() || options->rx.has() || options->rts.has() || options->cts.has();
    if (pinGiven) {
        js::fail<js::TypeError>("Serial: tx, rx, rts and cts are pins of a board, not of a port of the PC");
        return false;
    }

    settings->tx = hal::serial::NO_PIN;
    settings->rx = hal::serial::NO_PIN;
    settings->rts = hal::serial::NO_PIN;
    settings->cts = hal::serial::NO_PIN;
    return readFlowControl(&options->flowControl, &settings->flowControl);
}

/**
 * An open serial port (JS class Serial), made by 'new Serial(port, options)'.
 * Life cycle: init() opens the port, registers the message source, starts the
 * reader thread and keeps the object alive; close() only asks the reader
 * thread to stop; its MESSAGE_CLOSED removes the source, drops the keep-alive
 * and calls onClose. The finalizer asks the same, so a port left open is
 * freed at the end of the run.
 */
class Serial {
    JS_CLASS(Serial);

public:
    /**
     * Finalizer (no JS here): asks the reader thread to stop if it still
     * runs, and removes the source.
     */
    ~Serial() {
        requestClose();
        if (_loop != nullptr) {
            _loop->removeSource(_source);
        }
    }

    /**
     * JS constructor: new Serial(port, options) opens the port: a name on the
     * PC, a UART number on a board. Throws if it cannot be opened.
     */
    void init(js::Value port, js::Optional<SerialOptions> options) {
        // a driver must be there (every executable sets one)
        if (driver == nullptr) {
            return (void)js::fail("Serial: no serial ports here");
        }

        // the port, as the driver names them
        if (!readPort(port)) {
            return;
        }

        // the options, checked against the driver
        SerialOptions defaults;
        const SerialOptions *given = options.has() ? &options.value() : &defaults;
        hal::serial::Settings settings;
        if (!readSettings(given, &settings)) {
            return;
        }

        // a port of the PC must be granted
        if (driver->namedPorts && !serialAllowed(_name)) {
            return;
        }

        start(&settings);
    }

    /**
     * JS: callback(bytes) for each block received (a new Uint8Array).
     */
    JS_EXPORT(onData) void onData(js::Function callback) {
        _dataCallback = callback;
    }

    /**
     * JS: callback() once the port is closed (by close() or after an error).
     */
    JS_EXPORT(onClose) void onClose(js::Function callback) {
        _closeCallback = callback;
    }

    /**
     * JS: writes a string (UTF-8) or bytes, synchronously on the JS thread.
     * Throws TypeError for a wrong type, a system error (EIO) if the write failed.
     */
    JS_EXPORT(write) void write(js::Value data) {
        // the port must be open, and still owned by this side (close() lets
        // the state go at once)
        if (!_open || _state == nullptr) {
            return (void)js::fail("Serial: port closed");
        }

        // get the bytes: a C string of the JS string, or the byte view in place
        JSContext *ctx = js::context();
        const char *text = nullptr;
        JSValue buffer = JS_UNDEFINED;
        uint8_t *bytes = nullptr;
        size_t size = 0;
        bool ok = false;
        if (JS_IsString(data.get())) {
            text = JS_ToCStringLen(ctx, &size, data.get());
            bytes = (uint8_t *)((char *)text);
            ok = text != nullptr;
        } else {
            ok = js::detail::bytesOf(ctx, data.get(), &bytes, &size, &buffer);
        }

        // write them, then release what was taken; a wrong type is reported
        // after the release
        bool written = ok && driver->write(_state->port, bytes, size);
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, buffer);
        if (!ok) {
            return (void)js::fail<js::TypeError>("Serial: write: data must be a string or bytes");
        }

        // the write failed
        if (!written) {
            return (void)js::failSystem(EIO, _name);
        }
    }

    /**
     * JS: asks the reader thread to stop; it closes the port within 100 ms.
     * The close callback is called when the reader thread has stopped.
     */
    JS_EXPORT(close) void close() {
        requestClose();
    }

    /**
     * JS: the port given to the constructor: its name on the PC, its number
     * on a board.
     */
    JS_EXPORT(port) js::Value port() const {
        JSContext *ctx = js::context();
        JSValue value = driver->namedPorts ? JS_NewString(ctx, _name) : JS_NewInt32(ctx, _number);
        return js::Value(ctx, value);
    }

    /**
     * JS: false once the close message has been handled.
     */
    JS_EXPORT(isOpen) bool isOpen() const {
        return _open;
    }

private:
    /**
     * Reads the port argument: a string for a named driver, an integer for a
     * numbered one ("UART <n>" kept as its name for the messages). Throws a
     * TypeError and returns false otherwise.
     */
    bool readPort(js::Value port) {
        JSContext *ctx = js::context();

        // the PC: a name
        if (driver->namedPorts) {
            const char *name = JS_IsString(port.get()) ? JS_ToCString(ctx, port.get()) : nullptr;
            if (name == nullptr) {
                js::fail<js::TypeError>("Serial: the port must be a name, such as \"COM3\"");
                return false;
            }

            snprintf(_name, sizeof(_name), "%s", name);
            JS_FreeCString(ctx, name);
            return true;
        }

        // a board: the number of a UART
        int32_t number = -1;
        bool isInteger = JS_IsNumber(port.get()) && JS_ToInt32(ctx, &number, port.get()) == 0;
        double exact = 0;
        JS_ToFloat64(ctx, &exact, port.get());
        if (!isInteger || number < 0 || (double)number != exact) {
            js::fail<js::TypeError>("Serial: the port must be the number of a UART, such as 1");
            return false;
        }

        _number = (int)number;
        snprintf(_name, sizeof(_name), "UART %d", _number);
        return true;
    }

    /**
     * Opens the port and starts reading (JS thread). Returns false with a
     * pending error.
     */
    bool start(const hal::serial::Settings *settings) {
        // open and configure the port (fails with a system error)
        int error = 0;
        hal::serial::Port *opened = driver->open(_name, _number, settings, &error);
        if (opened == nullptr) {
            js::failSystem(error, _name);
            return false;
        }

        // register the message source and allocate the PortState shared with
        // the reader thread (the mailbox is retained for the thread)
        _loop = rt::Loop::current();
        _source = _loop->addSource(&onMessage, this);
        _state = (PortState *)memory::allocate(sizeof(PortState));
        *_state = PortState{ opened, 0, 2, _loop->mailbox(), _source };
        _state->mailbox->retain();

        // open: the object stays alive until the close message
        _open = true;
        js::keepAlive(this, true);

        // start the reader thread, which owns its share of the PortState from
        // then on; if it cannot start, close the port, undo the above (except
        // the source, removed by the destructor) and fail
        if (!platform::startThread(&readLoop, _state, platform::defaultThreadOptions("serial-read"))) {
            driver->close(opened);
            _state->mailbox->release();
            memory::release(_state);
            _state = nullptr;
            _open = false;
            js::keepAlive(this, false);
            js::fail("Serial: cannot start a thread");
            return false;
        }

        return true;
    }

    /**
     * Sets the 'closing' flag read by the reader thread and lets this side's
     * share of the PortState go (the thread, last owner, closes the port).
     * Does nothing if already done.
     */
    void requestClose() {
        if (_state != nullptr) {
            __atomic_store_n(&_state->closing, 1, __ATOMIC_RELEASE);
            releasePortState(_state);   // the reader thread closes the port when it stops
            _state = nullptr;
        }
    }

    /**
     * Messages of the reader thread, on the JS thread: data, then the close.
     */
    static void onMessage(void *opaque, const rt::Message *message) {
        // MESSAGE_DATA: onData with a copy of the block (the loop frees the
        // block after this call). Called through a local copy of the
        // callback: the callback may replace itself (port.onData(...)), which
        // would free the function while it runs
        Serial *serial = (Serial *)opaque;
        if (message->kind == MESSAGE_DATA) {
            js::Function callback = serial->_dataCallback;
            callback(js::Buffer::copy(message->data, message->size));
            return;
        }

        // MESSAGE_CLOSED: mark the port closed; after a read error the JS
        // side still owns its share of the state: let it go now; remove the
        // source
        serial->_open = false;
        if (serial->_state != nullptr) {
            releasePortState(serial->_state);
            serial->_state = nullptr;
        }

        serial->_loop->removeSource(serial->_source);

        // drop the keep-alive (the object may be collected after this), then
        // call onClose through a local copy of the callback, which stays valid
        // even if the object goes away
        js::Function callback = serial->_closeCallback;
        js::keepAlive(serial, false);
        callback();
    }

    char _name[256] = "";               // PC: the port's name; board: "UART <n>" (messages)
    int _number = -1;                   // board: the UART's number; PC: -1
    rt::Loop *_loop = nullptr;          // loop of the JS thread; nullptr before start()
    rt::SourceId _source = { -1, 0 };   // message source of the reader thread
    PortState *_state = nullptr;        // shared with the reader thread; nullptr once let go
    bool _open = false;                 // true from start() until the close message
    js::Function _dataCallback;         // onData callback; empty: none
    js::Function _closeCallback;        // onClose callback; empty: none
};

// The JS array list() fills.
struct PortList {
    JSValue  array;                     // owned by serialList()
    uint32_t count;                     // entries in it
};

/**
 * Appends a port given by the driver's list() to the PortList 'context'.
 */
void addToList(void *context, const char *name, int number) {
    PortList *list = (PortList *)context;
    JSContext *ctx = js::context();
    JSValue value = name != nullptr ? JS_NewString(ctx, name) : JS_NewInt32(ctx, number);
    JS_SetPropertyUint32(ctx, list->array, list->count++, value);
}

/**
 * JS list(): the ports that can be opened: names on the PC, numbers of the
 * free UARTs on a board (named serialList in C++, like the other module
 * functions).
 */
js::Value serialList() {
    if (driver == nullptr) {
        return js::fail("Serial: no serial ports here");
    }

    // listing the ports of the PC needs a port granted
    if (driver->namedPorts && !serialAllowed(nullptr)) {
        return js::Value();
    }

    // one entry per port
    JSContext *ctx = js::context();
    PortList list = { JS_NewArray(ctx), 0 };
    driver->list(&addToList, &list);
    return js::Value(ctx, list.array);
}

} // namespace

/**
 * Chooses the driver of the module (see hal_serial.h).
 */
void setSerialDriver(const hal::serial::Driver *serialDriver) {
    driver = serialDriver;
}

/**
 * Module "device/serial": list() and the class Serial.
 */
JS_MODULE(serial, module) {
    JS_FUNCTION_AS(module, serialList, "list");
    JS_ADD_CLASS(module, Serial);
}
