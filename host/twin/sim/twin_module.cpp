/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "qjsx:twinboard": what the twin's runtime reads of the simulated
// board and does to it (see twin.h): the server of the page
// (host/twin/js/server.js) and the models of the parts (qjsx:twin,
// host/twin/js/twin.js: pins driven and pulled, I2C answers, the bridge of
// the hardware events). Registered by "qjsx agent --board" only, for the
// twin's runtime (its own thread), never offered to the device program.
#include "jsbind.h"
#include "twin.h"
#include "hal_gpio.h"
#include "loop.h"
#include "memory.h"

namespace {

const int MAX_PINS = 64;          // as twin.cpp

/**
 * Name of a hal::gpio::Mode stored as a number in PinState, as device/gpio
 * spells it.
 */
const char *modeName(uint8_t mode) {
    switch ((hal::gpio::Mode)mode) {
        case hal::gpio::Mode::input: {
            return "input";
        }
        case hal::gpio::Mode::output: {
            return "output";
        }
        case hal::gpio::Mode::openDrain: {
            return "openDrain";
        }
    }

    return "input";
}

/**
 * Name of a hal::gpio::Pull stored as a number in PinState.
 */
const char *pullName(uint8_t pull) {
    switch ((hal::gpio::Pull)pull) {
        case hal::gpio::Pull::none: {
            return "none";
        }
        case hal::gpio::Pull::up: {
            return "up";
        }
        case hal::gpio::Pull::down: {
            return "down";
        }
    }

    return "none";
}

/**
 * JS board(): the board description, as a JSON text.
 */
js::Str board() {
    return js::Str(twin::boardJson());
}

/**
 * JS pins(): one object per pin, { configured, mode, pull, level, forced,
 * partDrive, partPull, conflict, rmt, i2c, adc, voltage } (forced, partDrive: 0, 1 or null;
 * partPull: "none", "up", "down"; conflict: the program and a component
 * drive different levels; rmt: a channel of device/rmt drives it; i2c: a
 * bus of device/i2c is open on it; adc: the program has its analog input
 * open; voltage: the voltage on it, in volts).
 */
js::Value pins() {
    JSContext *ctx = js::context();

    // the states, copied under the twin's lock
    twin::PinState states[MAX_PINS];
    int count = twin::pinCount();
    twin::snapshot(states, count);

    // one object each
    JSValue array = JS_NewArray(ctx);
    for (int i = 0; i < count; i++) {
        const twin::PinState *state = &states[i];
        JSValue pin = JS_NewObject(ctx);
        JSValue forced = state->forced < 0 ? JS_NULL : JS_NewInt32(ctx, state->forced);
        JSValue partDrive = state->partDrive < 0 ? JS_NULL : JS_NewInt32(ctx, state->partDrive);
        JS_SetPropertyStr(ctx, pin, "configured", JS_NewBool(ctx, state->configured));
        JS_SetPropertyStr(ctx, pin, "mode", JS_NewString(ctx, modeName(state->mode)));
        JS_SetPropertyStr(ctx, pin, "pull", JS_NewString(ctx, pullName(state->pull)));
        JS_SetPropertyStr(ctx, pin, "level", JS_NewInt32(ctx, state->level));
        JS_SetPropertyStr(ctx, pin, "forced", forced);
        JS_SetPropertyStr(ctx, pin, "partDrive", partDrive);
        JS_SetPropertyStr(ctx, pin, "partPull", JS_NewString(ctx, pullName(state->partPull)));
        JS_SetPropertyStr(ctx, pin, "conflict", JS_NewBool(ctx, state->conflict));
        JS_SetPropertyStr(ctx, pin, "rmt", JS_NewBool(ctx, state->rmt));
        JS_SetPropertyStr(ctx, pin, "i2c", JS_NewBool(ctx, state->i2c));
        JS_SetPropertyStr(ctx, pin, "adc", JS_NewBool(ctx, state->adc));
        JS_SetPropertyStr(ctx, pin, "voltage", JS_NewFloat64(ctx, twin::pinVoltage(i) / 1000.0));
        JS_SetPropertyUint32(ctx, array, (uint32_t)i, pin);
    }

    return js::Value(ctx, array);
}

/**
 * JS force(pin, level): the page drives a pin to 0 or 1, or lets it go (-1).
 */
void force(js::Range<int, 0, 63> pin, js::Range<int, -1, 1> level) {
    twin::force((int)pin, (int)level);
}

// ### The components (qjsx:twin, host/twin/js/twin.js) #######################

/**
 * JS pinDrive(pin, level): a component drives a pin to 0 or 1, or lets it
 * go (-1).
 */
void pinDrive(js::Range<int, 0, 63> pin, js::Range<int, -1, 1> level) {
    twin::partDrive((int)pin, (int)level);
}

/**
 * JS pinPull(pin, pull): a component pulls a pin: 0 none, 1 up, 2 down.
 */
void pinPull(js::Range<int, 0, 63> pin, js::Range<int, 0, 2> pull) {
    twin::partPull((int)pin, (int)pull);
}

/**
 * JS pinVoltage(pin, millivolts): a component sets the voltage on a pin
 * (an analog input reads it), or not any more (-1).
 */
void pinVoltage(js::Range<int, 0, 63> pin, js::Range<int, -1, 40000> millivolts) {
    twin::partVoltage((int)pin, (int)millivolts);
}

/**
 * JS pinWatch(pin, watched): the bridge tells the changes of the pin's level
 * (event "pin"), or not any more.
 */
void pinWatch(js::Range<int, 0, 63> pin, bool watched) {
    twin::partWatch((int)pin, watched);
}

/**
 * JS i2cAttach(sda, scl, address, attached): a component answers at
 * 'address' on the bus of those pins, or not any more.
 */
void i2cAttach(js::Range<int, 0, 63> sda, js::Range<int, 0, 63> scl, js::Range<int, 0, 127> address, bool attached) {
    twin::i2cAttach((int)sda, (int)scl, (int)address, attached);
}

/**
 * JS i2cAnswer(request, status, bytes): the answer to a transfer (event
 * "i2c"): status "ok" with the bytes read, "nack", or "failed".
 */
void i2cAnswer(double request, js::Str status, js::Optional<js::Bytes> bytes) {
    // the status as hal::i2c::Status
    hal::i2c::Status code = hal::i2c::Status::failed;
    if (status.equals("ok")) {
        code = hal::i2c::Status::ok;
    } else if (status.equals("nack")) {
        code = hal::i2c::Status::noAck;
    }

    // the bytes, when there are some
    const uint8_t *data = bytes.has() ? bytes.value().data() : nullptr;
    size_t size = bytes.has() ? bytes.value().size() : 0;
    twin::i2cAnswer((uint32_t)request, (int)code, data, size);
}

/**
 * JS ready(error): the parts are built (no argument), or not: why. The agent
 * waits for it before running the program.
 */
void ready(js::Optional<js::Str> error) {
    if (!error.has()) {
        twin::readyReport(nullptr);
        return;
    }

    // a copy ending with a 0 (a Str is a view)
    const js::Str *text = &error.value();
    char *copy = memory::copyText(text->data(), text->size());
    twin::readyReport(copy);
    memory::release(copy);
}

/**
 * A little-endian 32-bit number at 'bytes'.
 */
uint32_t readU32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/**
 * The JS event of a message of the bridge (see twin.h, BridgeEvent):
 *   { type: "pin", pin, level }
 *   { type: "rmtBytes", pin, bytes }
 *   { type: "rmtPulses", pin, pulses: [ticks, level, ...] }
 *   { type: "i2c", request, sda, scl, address, written, readLength }
 */
JSValue bridgeEvent(JSContext *ctx, const rt::Message *message) {
    JSValue event = JS_NewObject(ctx);
    switch ((twin::BridgeEvent)message->kind) {
        case twin::BridgeEvent::pinChange: {
            JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "pin"));
            JS_SetPropertyStr(ctx, event, "pin", JS_NewInt32(ctx, (int)message->value));
            JS_SetPropertyStr(ctx, event, "level", JS_NewInt32(ctx, message->size > 0 ? message->data[0] : 0));
            break;
        }
        case twin::BridgeEvent::rmtBytes: {
            js::Buffer bytes = js::Buffer::copy(message->data, message->size);
            JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "rmtBytes"));
            JS_SetPropertyStr(ctx, event, "pin", JS_NewInt32(ctx, (int)message->value));
            JS_SetPropertyStr(ctx, event, "bytes", bytes.value().dup());
            break;
        }
        case twin::BridgeEvent::rmtPulses: {
            // [ticks, level, ticks, level, ...]
            JSValue pulses = JS_NewArray(ctx);
            size_t count = message->size / 5;
            for (size_t i = 0; i < count; i++) {
                const uint8_t *entry = message->data + i * 5;
                JS_SetPropertyUint32(ctx, pulses, (uint32_t)(2 * i), JS_NewUint32(ctx, readU32(entry)));
                JS_SetPropertyUint32(ctx, pulses, (uint32_t)(2 * i + 1), JS_NewInt32(ctx, entry[4]));
            }

            JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "rmtPulses"));
            JS_SetPropertyStr(ctx, event, "pin", JS_NewInt32(ctx, (int)message->value));
            JS_SetPropertyStr(ctx, event, "pulses", pulses);
            break;
        }
        case twin::BridgeEvent::i2cTransfer: {
            // [sda, scl, address, 0, readSize u32 LE, written...]
            const uint8_t *data = message->data;
            js::Buffer written = js::Buffer::copy(data + 8, message->size - 8);
            JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "i2c"));
            JS_SetPropertyStr(ctx, event, "request", JS_NewFloat64(ctx, (double)message->value));
            JS_SetPropertyStr(ctx, event, "sda", JS_NewInt32(ctx, data[0]));
            JS_SetPropertyStr(ctx, event, "scl", JS_NewInt32(ctx, data[1]));
            JS_SetPropertyStr(ctx, event, "address", JS_NewInt32(ctx, data[2]));
            JS_SetPropertyStr(ctx, event, "written", written.value().dup());
            JS_SetPropertyStr(ctx, event, "readLength", JS_NewUint32(ctx, readU32(data + 4)));
            break;
        }
    }

    return event;
}

/**
 * JS new Bridge(callback): the bridge of the twin's runtime (one at a
 * time); callback(event) for each event of the simulated hardware (see
 * bridgeEvent), on this runtime's thread. Kept alive until close().
 */
class Bridge {
    JS_CLASS(Bridge);

public:
    /**
     * Finalizer (no JS here): no bridge any more, the source removed.
     */
    ~Bridge() {
        detach();
    }

    /**
     * JS constructor: registers the source and the bridge, keeps the object
     * alive.
     */
    void init(js::Function callback) {
        _callback = callback;
        _ctx = js::context();
        _loop = rt::Loop::current();
        _source = _loop->addSource(&handle, this);
        twin::bridgeAttach(_loop->mailbox(), _source);
        js::keepAlive(this, true);
    }

    /**
     * JS: no more events; the object may be collected.
     */
    JS_EXPORT(close) void close() {
        detach();
        js::keepAlive(this, false);
    }

private:
    /**
     * Lets the bridge go, once.
     */
    void detach() {
        if (_loop == nullptr) {
            return;
        }

        twin::bridgeDetach();
        _loop->removeSource(_source);
        _loop = nullptr;
    }

    /**
     * A message of the bridge (JS thread): the callback with its event,
     * through a local copy (the callback may close the bridge).
     */
    static void handle(void *opaque, const rt::Message *message) {
        Bridge *bridge = (Bridge *)opaque;
        JSContext *ctx = bridge->_ctx;
        js::Value event(ctx, bridgeEvent(ctx, message));
        js::Function callback = bridge->_callback;
        callback(event);
    }

    js::Function _callback;                  // called with each event
    JSContext   *_ctx = nullptr;             // context of the twin's runtime (set by init)
    rt::Loop    *_loop = nullptr;            // loop of the twin's runtime; nullptr: closed
    rt::SourceId _source = { -1, 0 };        // the bridge's source in that loop
};

/**
 * A pin of a UART for the page: its number, or null when not wired.
 */
JSValue pinValue(JSContext *ctx, int pin) {
    return pin == hal::serial::NO_PIN ? JS_NULL : JS_NewInt32(ctx, pin);
}

/**
 * Name of a hal::serial::Parity, as device/serial spells it.
 */
const char *parityName(hal::serial::Parity parity) {
    switch (parity) {
        case hal::serial::Parity::even: {
            return "even";
        }
        case hal::serial::Parity::odd: {
            return "odd";
        }
        default: {
            return "none";
        }
    }
}

/**
 * The JS object of the state of a TCP link: { port, listen, listening,
 * client, error } (error: why the last listening failed, "" if it did not).
 */
JSValue tcpValue(JSContext *ctx, const twin::TcpState *state) {
    JSValue tcp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tcp, "port", JS_NewInt32(ctx, state->port));
    JS_SetPropertyStr(ctx, tcp, "listen", JS_NewBool(ctx, state->listen));
    JS_SetPropertyStr(ctx, tcp, "listening", JS_NewBool(ctx, state->listening));
    JS_SetPropertyStr(ctx, tcp, "client", JS_NewBool(ctx, state->client));
    JS_SetPropertyStr(ctx, tcp, "error", JS_NewString(ctx, state->error));
    return tcp;
}

/**
 * JS uarts(): one object per UART of the board, { number, link, open, tcp,
 * baudRate, dataBits, parity, stopBits, flowControl, tx, rx, rts, cts } (tcp:
 * see tcpValue; the settings are those of the last open; a pin not wired is
 * null).
 */
js::Value uarts() {
    JSContext *ctx = js::context();

    // the states, copied under the locks of the UARTs
    twin::UartState states[twin::MAX_UARTS];
    int count = twin::uartCount();
    twin::uartStates(states, count);

    // one object each
    JSValue array = JS_NewArray(ctx);
    for (int i = 0; i < count; i++) {
        const twin::UartState *state = &states[i];
        const hal::serial::Settings *settings = &state->settings;
        JSValue uart = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, uart, "number", JS_NewInt32(ctx, i));
        JS_SetPropertyStr(ctx, uart, "link", JS_NewBool(ctx, state->link));
        JS_SetPropertyStr(ctx, uart, "open", JS_NewBool(ctx, state->open));
        JS_SetPropertyStr(ctx, uart, "tcp", tcpValue(ctx, &state->tcp));
        JS_SetPropertyStr(ctx, uart, "baudRate", JS_NewInt32(ctx, settings->baudRate));
        JS_SetPropertyStr(ctx, uart, "dataBits", JS_NewInt32(ctx, settings->dataBits));
        JS_SetPropertyStr(ctx, uart, "parity", JS_NewString(ctx, parityName(settings->parity)));
        JS_SetPropertyStr(ctx, uart, "stopBits", JS_NewInt32(ctx, settings->stopBits));
        JS_SetPropertyStr(ctx, uart, "flowControl", JS_NewBool(ctx, settings->flowControl));
        JS_SetPropertyStr(ctx, uart, "tx", pinValue(ctx, settings->tx));
        JS_SetPropertyStr(ctx, uart, "rx", pinValue(ctx, settings->rx));
        JS_SetPropertyStr(ctx, uart, "rts", pinValue(ctx, settings->rts));
        JS_SetPropertyStr(ctx, uart, "cts", pinValue(ctx, settings->cts));
        JS_SetPropertyUint32(ctx, array, (uint32_t)i, uart);
    }

    return js::Value(ctx, array);
}

// The JS array uartTraffic() fills.
struct TrafficList {
    JSValue  array;                 // owned by uartTraffic()
    uint32_t count;                 // records in it
};

/**
 * Appends a record of the traffic to the TrafficList 'context'.
 */
void addRecord(void *context, uint32_t sequence, twin::UartSource source, const uint8_t *bytes, size_t size) {
    TrafficList *list = (TrafficList *)context;
    JSContext *ctx = js::context();

    // who wrote it
    const char *sourceName = source == twin::UartSource::program ? "program"
                           : source == twin::UartSource::page    ? "page"
                                                                 : "tcp";

    // { sequence, source, bytes }
    js::Buffer copy = js::Buffer::copy(bytes, size);
    JSValue record = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, record, "sequence", JS_NewUint32(ctx, sequence));
    JS_SetPropertyStr(ctx, record, "source", JS_NewString(ctx, sourceName));
    JS_SetPropertyStr(ctx, record, "bytes", copy.value().dup());
    JS_SetPropertyUint32(ctx, list->array, list->count++, record);
}

/**
 * JS uartTraffic(uart, after): { sequence, records } — the records of the
 * UART numbered after 'after', each { sequence, source: "program" | "page" |
 * "tcp", bytes }; sequence is the number of its newest record.
 */
js::Value uartTraffic(js::Range<int, 0, twin::MAX_UARTS - 1> uart, double after) {
    JSContext *ctx = js::context();

    // a record number (a uint32_t); anything else: from the first record
    bool valid = after >= 0 && after <= 4294967295.0;
    uint32_t from = valid ? (uint32_t)after : 0;

    // the records
    TrafficList list = { JS_NewArray(ctx), 0 };
    uint32_t newest = twin::uartTraffic((int)uart, from, &addRecord, &list);

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "sequence", JS_NewUint32(ctx, newest));
    JS_SetPropertyStr(ctx, result, "records", list.array);
    return js::Value(ctx, result);
}

/**
 * JS i2cBuses(): one object per bus, { bus, open, sda, scl, frequencies }
 * (sda, scl null when closed; frequencies: of the devices on it, in Hz).
 */
js::Value i2cBuses() {
    JSContext *ctx = js::context();
    twin::I2cBusState states[twin::I2C_BUSES];
    twin::i2cBusStates(states);

    JSValue array = JS_NewArray(ctx);
    for (int i = 0; i < twin::I2C_BUSES; i++) {
        const twin::I2cBusState *state = &states[i];
        JSValue bus = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, bus, "bus", JS_NewInt32(ctx, i));
        JS_SetPropertyStr(ctx, bus, "open", JS_NewBool(ctx, state->open));
        JS_SetPropertyStr(ctx, bus, "sda", state->open ? JS_NewInt32(ctx, state->sda) : JS_NULL);
        JS_SetPropertyStr(ctx, bus, "scl", state->open ? JS_NewInt32(ctx, state->scl) : JS_NULL);
        JSValue frequencies = JS_NewArray(ctx);
        for (int k = 0; k < state->frequencyCount; k++) {
            JS_SetPropertyUint32(ctx, frequencies, (uint32_t)k, JS_NewUint32(ctx, state->frequencies[k]));
        }
        JS_SetPropertyStr(ctx, bus, "frequencies", frequencies);
        JS_SetPropertyUint32(ctx, array, (uint32_t)i, bus);
    }

    return js::Value(ctx, array);
}

/**
 * The name of a hal::i2c::Status as a number, for the page.
 */
const char *i2cStatusName(int status) {
    switch ((hal::i2c::Status)status) {
        case hal::i2c::Status::ok: {
            return "ok";
        }
        case hal::i2c::Status::noAck: {
            return "nack";
        }
        case hal::i2c::Status::timeout: {
            return "timeout";
        }
        case hal::i2c::Status::failed: {
            return "failed";
        }
    }

    return "failed";
}

/**
 * Adds a record of the I2C trace to the array of a TrafficList (lock of the
 * trace held: no JS but building values).
 */
void addI2cRecord(void *context, const twin::I2cRecord *record) {
    TrafficList *list = (TrafficList *)context;
    JSContext *ctx = js::context();

    // the bytes kept: what was written, what came back (ok only)
    size_t writeKept = record->writeSize < twin::I2C_TRACE_BYTES ? record->writeSize : twin::I2C_TRACE_BYTES;
    size_t readKept = record->readSize < twin::I2C_TRACE_BYTES ? record->readSize : twin::I2C_TRACE_BYTES;
    bool answered = record->status == (int)hal::i2c::Status::ok;
    js::Buffer written = js::Buffer::copy(record->written, writeKept);
    js::Buffer read = js::Buffer::copy(record->read, answered ? readKept : 0);

    // { sequence, time, bus, address, status, writeSize, readSize, written, read }
    JSValue item = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, item, "sequence", JS_NewUint32(ctx, record->sequence));
    JS_SetPropertyStr(ctx, item, "time", JS_NewFloat64(ctx, (double)record->timeMs));
    JS_SetPropertyStr(ctx, item, "bus", JS_NewInt32(ctx, record->bus));
    JS_SetPropertyStr(ctx, item, "address", JS_NewInt32(ctx, record->address));
    JS_SetPropertyStr(ctx, item, "status", JS_NewString(ctx, i2cStatusName(record->status)));
    JS_SetPropertyStr(ctx, item, "frequency", JS_NewUint32(ctx, record->frequencyHz));
    JS_SetPropertyStr(ctx, item, "writeSize", JS_NewUint32(ctx, (uint32_t)record->writeSize));
    JS_SetPropertyStr(ctx, item, "readSize", JS_NewUint32(ctx, (uint32_t)record->readSize));
    JS_SetPropertyStr(ctx, item, "written", written.value().dup());
    JS_SetPropertyStr(ctx, item, "read", read.value().dup());
    JS_SetPropertyUint32(ctx, list->array, list->count++, item);
}

/**
 * JS i2cTrace(after): { sequence, records } — the transfers numbered after
 * 'after' (every bus), each { sequence, time (ms), bus, address, status:
 * "ok" | "nack" | "timeout" | "failed", frequency (Hz; 0: a probe),
 * writeSize, readSize, written, read (the first bytes) }| "nack" | "timeout" | "failed", writeSize, readSize, written, read
 * (the first bytes) }; sequence is the number of the newest.
 */
js::Value i2cTrace(double after) {
    JSContext *ctx = js::context();

    // a record number (a uint32_t); anything else: from the first record
    bool valid = after >= 0 && after <= 4294967295.0;
    uint32_t from = valid ? (uint32_t)after : 0;

    TrafficList list = { JS_NewArray(ctx), 0 };
    uint32_t newest = twin::i2cTrace(from, &addI2cRecord, &list);

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "sequence", JS_NewUint32(ctx, newest));
    JS_SetPropertyStr(ctx, result, "records", list.array);
    return js::Value(ctx, result);
}

/**
 * JS uartSend(uart, bytes): the page sends bytes to the program; false if
 * the program does not have the UART open.
 */
bool uartSend(js::Range<int, 0, twin::MAX_UARTS - 1> uart, js::Bytes bytes) {
    return twin::uartSend((int)uart, bytes.data(), bytes.size());
}

/**
 * JS usb(): the USB device, { started, manufacturer, product, serialNumber,
 * vendorId, productId, serials, functions: [{ tcp }] } (the descriptors of
 * the last start; serials: functions of the started device, 0 when
 * stopped; one entry per possible function, see tcpValue).
 */
js::Value usb() {
    JSContext *ctx = js::context();

    // the state, copied under the locks
    twin::UsbState state;
    twin::usbState(&state);
    const hal::usb::Descriptors *descriptors = &state.descriptors;

    // the device
    JSValue device = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, device, "started", JS_NewBool(ctx, state.started));
    JS_SetPropertyStr(ctx, device, "manufacturer", JS_NewString(ctx, descriptors->manufacturer));
    JS_SetPropertyStr(ctx, device, "product", JS_NewString(ctx, descriptors->product));
    JS_SetPropertyStr(ctx, device, "serialNumber", JS_NewString(ctx, descriptors->serialNumber));
    JS_SetPropertyStr(ctx, device, "vendorId", JS_NewInt32(ctx, descriptors->vendorId));
    JS_SetPropertyStr(ctx, device, "productId", JS_NewInt32(ctx, descriptors->productId));
    JS_SetPropertyStr(ctx, device, "serials", JS_NewInt32(ctx, state.serials));

    // its functions
    JSValue functions = JS_NewArray(ctx);
    for (int i = 0; i < hal::usb::MAX_SERIALS; i++) {
        JSValue function = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, function, "tcp", tcpValue(ctx, &state.tcp[i]));
        JS_SetPropertyUint32(ctx, functions, (uint32_t)i, function);
    }

    JS_SetPropertyStr(ctx, device, "functions", functions);
    return js::Value(ctx, device);
}

/**
 * JS tcpListen(key, port, listen): the page chooses the port of the TCP
 * link 'key' ("uart1", "usb0") and turns its listening on or off; applied
 * by the twin within 10 ms. False if there is no such link.
 */
bool tcpListen(js::Str key, js::Range<int, 0, 65535> port, bool listen) {
    twin::TcpLink *link = twin::findTcpLink(key.data());
    if (link == nullptr) {
        return false;
    }

    link->request((int)port, listen);
    return true;
}

/**
 * JS uiArchive(): the web page of the twin, a zip archive with stored
 * entries (Uint8Array, a copy of what the executable embeds).
 */
js::Buffer uiArchive() {
    size_t size = 0;
    const uint8_t *archive = twin::uiArchive(&size);
    return js::Buffer::copy(archive, size);
}

} // namespace

/**
 * Module "qjsx:twinboard".
 */
JS_MODULE(twinboard, module) {
    JS_FUNCTION(module, board);
    JS_FUNCTION(module, pins);
    JS_FUNCTION(module, force);
    JS_FUNCTION(module, pinDrive);
    JS_FUNCTION(module, pinPull);
    JS_FUNCTION(module, pinWatch);
    JS_FUNCTION(module, pinVoltage);
    JS_FUNCTION(module, i2cAttach);
    JS_FUNCTION(module, i2cAnswer);
    JS_FUNCTION(module, ready);
    JS_FUNCTION(module, uarts);
    JS_FUNCTION(module, uartTraffic);
    JS_FUNCTION(module, uartSend);
    JS_FUNCTION(module, i2cBuses);
    JS_FUNCTION(module, i2cTrace);
    JS_FUNCTION(module, usb);
    JS_FUNCTION(module, tcpListen);
    JS_FUNCTION(module, uiArchive);
    JS_ADD_CLASS(module, Bridge);
}
