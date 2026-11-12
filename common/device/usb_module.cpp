/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "device/usb": the board as a USB device for a host (a PC, a
// tablet), through the hardware of the port (hal_usb.h), on the boards whose
// port implements it (registered by the port, see device_modules.h).
//
//   import { Usb } from "device/usb";
//   const usb = new Usb({ manufacturer: "Acme", product: "Gadget", serialNumber: "v2.0.0" });
//   const port = usb.serial();                 // a serial function (UsbCdc), declared before start()
//   port.onData(bytes => ...);                 // Uint8Array
//   port.onLineState(({ dtr, rts }) => ...);   // the host opened / closed the port
//   usb.start();                               // the host sees the device
//   port.write("hello\r\n");                   // string or bytes
//   port.lineState();                          // { dtr, rts } now
//   usb.close();                               // the host sees it go away
//
// A composite device: the functions are declared with serial() (HID, mass
// storage... later) before start(), which presents them all at once. One Usb
// object at a time; a started one keeps the program running until close().
//
// The device outlives the program: at the end of a program it stays started
// (the bytes received meanwhile are dropped). The next program that starts a
// device with the same descriptors and functions takes it over without the
// host seeing it go away; other descriptors restart it. Only close() stops it.
//
// The events of the hal come through a single target (mailbox + source),
// never through a pointer to an object.
#include <errno.h>
#include <string.h>

#include "jsbind.h"
#include "hal_usb.h"
#include "loop.h"
#include "memory.h"
#include "platform.h"

namespace {

// Options of 'new Usb(options)'.
struct UsbOptions {
    js::Optional<js::Str> manufacturer;            // default: DEFAULT_MANUFACTURER
    js::Optional<js::Str> product;                 // default: DEFAULT_PRODUCT
    js::Optional<js::Str> serialNumber;            // default: none
    js::Range<int, 0, 0xffff> vendorId = 0;        // 0: the default of the chip's vendor
    js::Range<int, 0, 0xffff> productId = 0;       // 0: the default of the chip's vendor
};
JS_STRUCT(UsbOptions, manufacturer, product, serialNumber, vendorId, productId);

const char *const DEFAULT_MANUFACTURER = "qjsx";
const char *const DEFAULT_PRODUCT = "qjsx device";

const int MESSAGE_DATA = 1;          // bytes received; value: the serial function
const int MESSAGE_LINE_STATE = 2;    // line state changed; value: serial function | DTR_FLAG | RTS_FLAG
const int SERIAL_MASK = 0xff;        // the serial function in the value of a message
const int DTR_FLAG = 1 << 8;         // DTR set, in the value of MESSAGE_LINE_STATE
const int RTS_FLAG = 1 << 9;         // RTS set, in the value of MESSAGE_LINE_STATE

// Where the events of the device go (JS thread writes, task of the hal
// reads, under targetLock).
struct Target {
    rt::Mailbox *mailbox;  // mailbox of the loop of the run, retained; nullptr: nobody, drop
    rt::SourceId source;   // the message source of the started Usb object
};

Target target = { nullptr, { -1, 0 } };   // targetLock
platform::Mutex targetLock;              // target

// The device as the hal runs it: kept between programs (JS thread only).
bool deviceStarted = false;                      // hal::usb::start() succeeded, no stop() since
hal::usb::Descriptors startedDescriptors;        // what it was started with
int startedSerials = 0;                          // how many serial functions it has

// The Usb objects (JS thread only): each gets a generation number; a UsbCdc
// keeps the one of its Usb, so it can tell whether its device is the started
// one without a pointer to it.
uint32_t lastGeneration = 0;           // generation given to the last Usb made
uint32_t startedGeneration = 0;        // generation of the Usb that started the device; 0: none
const void *existingUsb = nullptr;     // the Usb object not closed nor collected yet; nullptr: none

/**
 * Handler of the received bytes (task of the hal): posts them to the started
 * Usb object, or drops them when there is none.
 */
void onReceived(int serial, uint8_t *data, size_t size) {
    targetLock.lock();
    if (target.mailbox != nullptr) {
        target.mailbox->post(target.source, MESSAGE_DATA, data, size, serial);
    } else {
        memory::release(data);
    }
    targetLock.unlock();
}

/**
 * Handler of the line state changes (task of the hal): posts them to the
 * started Usb object, if any.
 */
void onLineChanged(int serial, bool dtr, bool rts) {
    int64_t value = serial | (dtr ? DTR_FLAG : 0) | (rts ? RTS_FLAG : 0);

    targetLock.lock();
    if (target.mailbox != nullptr) {
        target.mailbox->post(target.source, MESSAGE_LINE_STATE, nullptr, 0, value);
    }
    targetLock.unlock();
}

/**
 * Copies a descriptor text into 'out' (MAX_TEXT + 1 bytes): 'text' if given,
 * else 'fallback'. False with a RangeError pending if it is not printable
 * ASCII of at most MAX_TEXT characters.
 */
bool copyText(const js::Optional<js::Str> *text, const char *fallback, const char *field, char *out) {
    // the text given, or the default
    const char *data = fallback;
    size_t size = strlen(fallback);
    if (text->has()) {
        data = text->value().data();
        size = text->value().size();
    }

    // printable ASCII (what the USB string descriptors of the hal carry)
    bool printable = size <= (size_t)hal::usb::MAX_TEXT;
    for (size_t i = 0; i < size && printable; i++) {
        printable = data[i] >= 0x20 && data[i] <= 0x7e;
    }
    if (!printable) {
        js::fail<js::RangeError>("usb: %s: at most %d printable ASCII characters", field, hal::usb::MAX_TEXT);
        return false;
    }

    memcpy(out, data, size);
    out[size] = '\0';
    return true;
}

/**
 * The JS object { dtr, rts } of a line state.
 */
js::Value lineStateObject(bool dtr, bool rts) {
    JSContext *ctx = js::context();
    JSValue object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, object, "dtr", JS_NewBool(ctx, dtr));
    JS_SetPropertyStr(ctx, object, "rts", JS_NewBool(ctx, rts));
    return js::Value(ctx, object);
}

/**
 * A serial function of a USB device (JS class UsbCdc), made by usb.serial().
 */
class UsbCdc {
    JS_CLASS(UsbCdc);

public:
    /**
     * JS constructor: refused, a UsbCdc comes from usb.serial().
     */
    void init() {
        return (void)js::fail<js::TypeError>("usb: a UsbCdc comes from usb.serial()");
    }

    /**
     * JS: callback(bytes) for each block received (a new Uint8Array).
     */
    JS_EXPORT(onData) void onData(js::Function callback) {
        _dataCallback = callback;
    }

    /**
     * JS: callback({ dtr, rts }) when the host changes the line state.
     */
    JS_EXPORT(onLineState) void onLineState(js::Function callback) {
        _lineStateCallback = callback;
    }

    /**
     * JS: sends a string (UTF-8) or bytes. What the host does not take in
     * time is dropped without error (nobody there, or not reading). Throws
     * if the device is not started (or closed), EIO if the hal refused.
     */
    JS_EXPORT(write) void write(js::Value data) {
        // the device of this function must be the started one
        if (_generation != startedGeneration) {
            return (void)js::fail("usb: the device is not started");
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

        // send them, then release what was taken; a wrong type is reported
        // after the release
        bool written = ok && hal::usb::write(_serial, bytes, size);
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, buffer);
        if (!ok) {
            return (void)js::fail<js::TypeError>("usb: write: data must be a string or bytes");
        }

        if (!written) {
            return (void)js::failSystem(EIO, "usb");
        }
    }

    /**
     * JS: the line state now, { dtr, rts } (both false when the device is
     * not started).
     */
    JS_EXPORT(lineState) js::Value lineState() {
        bool dtr = false;
        bool rts = false;
        if (_generation == startedGeneration) {
            hal::usb::lineState(_serial, &dtr, &rts);
        }
        return lineStateObject(dtr, rts);
    }

    /**
     * Ties the function to serial function 'serial' of the Usb object of
     * generation 'generation' (usb.serial()).
     */
    void bind(int serial, uint32_t generation) {
        _serial = serial;
        _generation = generation;
    }

    /**
     * Bytes received (JS thread): onData with a copy. Called through a local
     * copy of the callback, which may replace itself while it runs.
     */
    void deliverData(const uint8_t *data, size_t size) {
        js::Function callback = _dataCallback;
        callback(js::Buffer::copy(data, size));
    }

    /**
     * The line state changed (JS thread): onLineState, through a local copy
     * of the callback.
     */
    void deliverLineState(bool dtr, bool rts) {
        js::Function callback = _lineStateCallback;
        callback(lineStateObject(dtr, rts));
    }

private:
    int          _serial = 0;              // serial function of the hal
    uint32_t     _generation = 0;          // generation of its Usb object
    js::Function _dataCallback;            // onData callback; empty: none
    js::Function _lineStateCallback;       // onLineState callback; empty: none
};

/**
 * A USB device (JS class Usb).
 */
class Usb {
    JS_CLASS(Usb);

public:
    /**
     * Finalizer (no JS here): the events stop coming to this object; the
     * device itself stays started for the next program.
     */
    ~Usb() {
        if (_started) {
            stopEvents();
            startedGeneration = 0;
        }
        if (existingUsb == this) {
            existingUsb = nullptr;
        }
    }

    /**
     * JS constructor: new Usb({ manufacturer, product, serialNumber,
     * vendorId, productId }) describes the device. Throws if another Usb
     * object exists (close it first) or a text is refused.
     */
    void init(js::Optional<UsbOptions> options) {
        // one Usb object at a time
        if (existingUsb != nullptr) {
            return (void)js::fail("usb: busy: another Usb object exists (close it first)");
        }

        // the descriptors, zero-filled so that two of them compare with memcmp
        UsbOptions defaults;
        const UsbOptions *settings = options.has() ? &options.value() : &defaults;
        memset(&_descriptors, 0, sizeof(_descriptors));
        _descriptors.vendorId = (uint16_t)(int)settings->vendorId;
        _descriptors.productId = (uint16_t)(int)settings->productId;

        bool valid = copyText(&settings->manufacturer, DEFAULT_MANUFACTURER, "manufacturer", _descriptors.manufacturer)
                     && copyText(&settings->product, DEFAULT_PRODUCT, "product", _descriptors.product)
                     && copyText(&settings->serialNumber, "", "serialNumber", _descriptors.serialNumber);
        if (!valid) {
            return;
        }

        _generation = ++lastGeneration;
        existingUsb = this;
    }

    /**
     * JS: declares a serial function (CDC) and returns it (a UsbCdc). Only
     * before start(), at most hal::usb::MAX_SERIALS.
     */
    JS_EXPORT(serial) js::Value serial() {
        // the device must be open and not started yet
        if (!checkDeclaring()) {
            return js::Value();
        }
        if (_serialCount >= hal::usb::MAX_SERIALS) {
            return js::fail<js::RangeError>("usb: at most %d serial functions", hal::usb::MAX_SERIALS);
        }

        // the function, kept by this object (its events go through it)
        js::Created<UsbCdc> created = js::create<UsbCdc>();
        if (created.object == nullptr) {
            return js::Value();
        }

        created.object->bind(_serialCount, _generation);
        _serials[_serialCount] = created.object;
        _serialValues[_serialCount] = created.value;
        _serialCount++;
        return created.value;
    }

    /**
     * JS: presents the device and its functions to the host:
     * 1. the same device is already started (a previous program): it is
     *    taken over, the host sees nothing;
     * 2. else the started one (if any) is stopped and this one started;
     * 3. the events of the device come to this object, which keeps the
     *    program running until close().
     */
    JS_EXPORT(start) void start() {
        if (!checkDeclaring()) {
            return;
        }
        if (_serialCount == 0) {
            return (void)js::fail("usb: no function: declare one with usb.serial() first");
        }

        // 1-2. take the started device over, or start this one
        hal::usb::setHandlers(&onReceived, &onLineChanged);
        bool same = deviceStarted && startedSerials == _serialCount
                    && memcmp(&startedDescriptors, &_descriptors, sizeof(_descriptors)) == 0;
        if (!same) {
            if (deviceStarted) {
                hal::usb::stop();
                deviceStarted = false;
            }
            if (!hal::usb::start(&_descriptors, _serialCount)) {
                return (void)js::fail("usb: the hardware refused to start the device");
            }
            deviceStarted = true;
            startedDescriptors = _descriptors;
            startedSerials = _serialCount;
        }

        // 3. its events come here
        _loop = rt::Loop::current();
        _source = _loop->addSource(&onMessage, this);
        targetLock.lock();
        target = Target{ _loop->mailbox(), _source };
        target.mailbox->retain();
        targetLock.unlock();

        _started = true;
        startedGeneration = _generation;
        js::keepAlive(this, true);
    }

    /**
     * JS: stops the device (the host sees it go away) and frees the place
     * for another Usb object. This object can no longer be used.
     */
    JS_EXPORT(close) void close() {
        if (_started) {
            stopEvents();
            hal::usb::stop();
            deviceStarted = false;
            startedGeneration = 0;
            _started = false;
            js::keepAlive(this, false);
        }

        _closed = true;
        if (existingUsb == this) {
            existingUsb = nullptr;
        }
    }

private:
    /**
     * True if functions may still be declared (open, not started); else
     * false with an error pending.
     */
    bool checkDeclaring() {
        if (_closed) {
            js::fail("usb: closed");
            return false;
        }
        if (_started) {
            js::fail("usb: already started");
            return false;
        }
        return true;
    }

    /**
     * The events of the device stop coming to this object (no JS: also
     * called by the finalizer).
     */
    void stopEvents() {
        targetLock.lock();
        rt::Mailbox *mailbox = target.mailbox;
        target = Target{ nullptr, { -1, 0 } };
        targetLock.unlock();
        if (mailbox != nullptr) {
            mailbox->release();
        }

        _loop->removeSource(_source);
        _loop = nullptr;
    }

    /**
     * An event of the device (JS thread): to its serial function.
     */
    static void onMessage(void *opaque, const rt::Message *message) {
        Usb *usb = (Usb *)opaque;
        int serial = (int)(message->value & SERIAL_MASK);
        if (serial >= usb->_serialCount) {
            return;
        }

        UsbCdc *function = usb->_serials[serial];
        if (message->kind == MESSAGE_DATA) {
            function->deliverData(message->data, message->size);
        } else {
            bool dtr = (message->value & DTR_FLAG) != 0;
            bool rts = (message->value & RTS_FLAG) != 0;
            function->deliverLineState(dtr, rts);
        }
    }

    hal::usb::Descriptors _descriptors;                          // what the host will see
    uint32_t     _generation = 0;                                // number of this object (lastGeneration)
    int          _serialCount = 0;                               // serial functions declared
    UsbCdc      *_serials[hal::usb::MAX_SERIALS] = {};           // the declared functions, owned by JS
    js::Value    _serialValues[hal::usb::MAX_SERIALS];           // their JS objects, kept alive by this one
    bool         _started = false;                               // start() succeeded, no close() since
    bool         _closed = false;                                // close() was called
    rt::Loop    *_loop = nullptr;                                // loop of the run while started
    rt::SourceId _source = { -1, 0 };                            // message source of the events
};

} // namespace

/**
 * The "device/usb" module: the classes Usb and UsbCdc.
 */
JS_MODULE(usb, module) {
    JS_ADD_CLASS(module, Usb);
    JS_ADD_CLASS(module, UsbCdc);
}
