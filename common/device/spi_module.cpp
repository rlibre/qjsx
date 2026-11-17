/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "device/spi": SPI master buses and the devices on them, through the
// hardware of the port (hal_spi.h), on the boards whose port implements it
// (registered by the port, see device_modules.h).
//
//   import { Spi } from "device/spi";
//   const bus = new Spi(2, { sclk: 12, mosi: 11, miso: 13 });
//   const display = bus.device({ cs: 10, dc: 9, frequency: 20000000 });   // SpiDevice
//   await display.command([0x2a]);                              // DC low
//   await display.write([0, 0, 0, 127]);                        // DC high
//   const [id] = await sensor.transaction([{ write: [0x8f] }, { read: 1 }]);
//   const reply = sensor.writeReadSync([0x9f], 3);              // the same, blocking the JS thread
//   bus.close();                                                // closes its devices too
//
// Errors carry a Node-like 'code': EBADF (the bus or the device is closed),
// EAGAIN (16 transactions pending on the bus already), ECANCELED (dropped by
// close()), EIO (the hardware failed); opening a bus: ENOENT (no such port),
// EBUSY (taken), EINVAL (a pin the chip cannot use). Argument errors are
// TypeError / RangeError without code. A method that returns a promise never
// throws: every error, argument errors included, rejects its promise
// (promiseOrRejection below).
//
// Order: the transactions of a bus run one at a time, in the order of the
// calls. A promise starts its transaction in the hal (hal::spi::start); the
// hal's done callback only records the end and posts a message to the loop,
// which settles the promise and starts the next queued transaction of the
// bus. A Sync call first empties the bus: it waits for the started
// transaction (doneLock, without the loop), runs the queued ones on the JS
// thread (hal::spi::run) and settles their promises, then runs its own.
//
// Bytes: a queued transaction owns copies of its bytes (memory::allocate)
// until its end; a Sync call sends the program's bytes in place and reads
// straight into the Uint8Arrays it returns.
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "jsbind.h"
#include "hal_spi.h"
#include "spi_module.h"
#include "strict_int.h"
#include "loop.h"
#include "memory.h"
#include "platform.h"

namespace {

const int MAX_PORTS = 8;                    // port numbers this module can hold (the hal may have fewer)
const int MAX_DEVICES = 16;                 // devices of a bus
const int MAX_PENDING = 16;                 // promises of a bus not settled yet
const int MAX_SEGMENTS = 1024;              // segments of a transaction
const int MAX_SEGMENT_BYTES = 1048576;      // bytes of a segment (cut in pieces by the hal)
const int MAX_TRANSFER = 32768;             // largest maxTransfer of a bus (one DMA transfer)
const int MAX_DELAY_US = 1000;              // longest delay segment; longer waits: await between transactions
const int MESSAGE_DONE = 1;                 // the started transaction is over; value: job id | status << 32

// The integer arguments and options, checked strictly (strict_int.h).
using device::StrictInt;

// Options of 'new Spi(port, options)'.
struct BusOptions {
    js::Optional<StrictInt<0, 255>> sclk;                // clock pin (required)
    js::Optional<StrictInt<0, 255>> mosi;                // data out; absent: not wired
    js::Optional<StrictInt<0, 255>> miso;                // data in; absent: not wired
    StrictInt<1, MAX_TRANSFER>      maxTransfer = 4096;  // bytes of one transfer of the hardware (DMA)
};
JS_STRUCT(BusOptions, sclk, mosi, miso, maxTransfer);

// Bit order of a device (option bitOrder).
JS_ENUM(BitOrder, msb, lsb);

// Options of 'bus.device(options)'.
struct DeviceOptions {
    js::Optional<StrictInt<0, 255>> cs;                    // chip select pin; absent: the program drives it
    StrictInt<1000, 80000000>       frequency = 1000000;   // clock asked for, Hz
    StrictInt<0, 3>                 mode = 0;              // bit 1 = CPOL, bit 0 = CPHA
    BitOrder                        bitOrder = BitOrder::msb;   // order of the bits of a byte
    bool                            csActiveHigh = false;  // chip select active at level 1
    js::Optional<StrictInt<0, 255>> dc;                    // data / command pin; absent: none
    StrictInt<0, 255>               fill = 0;              // byte sent while reading
};
JS_STRUCT(DeviceOptions, cs, frequency, mode, bitOrder, csActiveHigh, dc, fill);

// One item of 'device.transaction(segments)': exactly one of write, read,
// transfer, delay (checked by draftOf), and an optional DC level.
struct SegmentOptions {
    js::Value                                         write;      // bytes to send; undefined: not this kind
    js::Optional<StrictInt<1, MAX_SEGMENT_BYTES>>     read;       // bytes to read
    js::Value                                         transfer;   // bytes to exchange (full duplex)
    js::Optional<StrictInt<0, MAX_DELAY_US>>          delay;      // microseconds, chip select kept
    js::Optional<StrictInt<0, 1>>                     dc;         // level of the DC pin; default 1 (data)
};
JS_STRUCT(SegmentOptions, write, read, transfer, delay, dc);

// What a segment does.
enum class Kind {
    write,                                  // sends bytes
    read,                                   // receives bytes, sending the fill byte
    transfer,                               // sends and receives at the same time
    delay,                                  // waits, chip select kept
};

// A segment as read from JS, before its bytes are placed (copied or viewed).
struct Draft {
    Kind     kind;                          // what it does
    JSValue  source;                        // write / transfer: the bytes given (owned reference); else JS_UNDEFINED
    size_t   readSize;                      // read: bytes to read
    uint32_t delayUs;                       // delay: microseconds
    int      dc;                            // level of the DC pin (0 or 1)
};

// What a transaction resolves its promise with.
enum class Answer {
    nothing,                                // write, command: undefined
    bytes,                                  // read, transfer, writeRead: the bytes of the reading segment
    list,                                   // transaction: one Uint8Array per read / transfer segment
};

// A transaction queued on its bus (or started), owned by the bus object.
struct Job {
    uint32_t           id;                  // number of the job in its bus object (finds its promise)
    int                device;              // hal device
    hal::spi::Segment *segments;            // memory::allocate, with the copies they point to; owned
    size_t             count;               // segments
    Job               *next;                // next in the queue of the bus
};

// Where the end of the started transaction of a port goes (JS thread writes,
// the hal's done callback reads and writes, under doneLock).
struct Completion {
    rt::Mailbox      *mailbox;              // mailbox of the loop of the run, retained; nullptr: port closed
    rt::SourceId      source;               // message source of the bus object
    uint32_t          jobId;                // the started job; 0: none
    bool              finished;             // its done callback came
    hal::spi::Status  status;               // what it said
};

Completion completions[MAX_PORTS] = {};     // by port; doneLock
platform::Signal doneLock;                  // completions; notified at each end of a started transaction

/**
 * The errno value of a status of the hal (0 for ok).
 */
int errorOf(hal::spi::Status status) {
    switch (status) {
        case hal::spi::Status::ok: {
            return 0;
        }
        case hal::spi::Status::noSuchPort: {
            return ENOENT;
        }
        case hal::spi::Status::busy: {
            return EBUSY;
        }
        case hal::spi::Status::badPin: {
            return EINVAL;
        }
        default: {
            return EIO;
        }
    }
}

/**
 * Done callback given to hal::spi::start(), on any thread (not an
 * interrupt: posting to a mailbox takes a lock and allocates): records the
 * end for a Sync call that waits, and posts it to the loop of the bus.
 * 'opaque' is the port number.
 */
void onTransactionDone(void *opaque, hal::spi::Status status) {
    // under the lock: the JS thread may wait for it, or close the port now
    int port = (int)(intptr_t)opaque;
    doneLock.lock();
    Completion *completion = &completions[port];
    completion->finished = true;
    completion->status = status;

    // to the loop: the job id in the low 32 bits, the status above
    if (completion->mailbox != nullptr) {
        int64_t value = (int64_t)completion->jobId | ((int64_t)status << 32);
        completion->mailbox->post(completion->source, MESSAGE_DONE, nullptr, 0, value);
    }

    doneLock.notify();
    doneLock.unlock();
}

// ### Promise methods that never throw #########################################

/**
 * What a method returning a promise gives back to JS: 'promise' when the
 * current call recorded no error, else a new promise rejected with that
 * error (taken out of the call, which then succeeds). So such a method never
 * throws: argument errors (TypeError, RangeError) reject as well.
 *
 * Written to be moved into jsbind for every module. Limit: QuickJS does not
 * tell whether the pending exception is uncatchable (an interrupt); the end
 * of the program (process.exit) is recognized through the loop and left
 * thrown.
 */
js::Value promiseOrRejection(const js::Promise<js::Value> &promise) {
    // no error: the promise itself
    if (!js::failed()) {
        return promise.value();
    }

    // the program is ending (process.exit): the exception stays thrown
    rt::Loop *loop = rt::Loop::current();
    bool exiting = loop != nullptr && loop->exitRequested();
    if (exiting) {
        return js::Value();
    }

    // the error leaves the call, which no longer fails
    JSContext *ctx = js::context();
    JSValue error = JS_GetException(ctx);
    js::detail::currentCall->failed = false;

    // a new promise, rejected with it
    JSValue functions[2];
    JSValue rejected = JS_NewPromiseCapability(ctx, functions);
    if (JS_IsException(rejected)) {
        JS_FreeValue(ctx, error);
        js::detail::markPendingException();
        return js::Value();
    }

    JSValue called = JS_Call(ctx, functions[1], JS_UNDEFINED, 1, &error);
    JS_FreeValue(ctx, called);
    JS_FreeValue(ctx, functions[0]);
    JS_FreeValue(ctx, functions[1]);
    JS_FreeValue(ctx, error);
    return js::Value(ctx, rejected);
}

/**
 * Converts argument 'argument' (1-based) of 'function' into *out, as jsbind
 * converts the parameters it binds; false with the error recorded. For the
 * methods returning a promise, which take their arguments as js::Value to
 * reject instead of throwing.
 */
template<class T>
bool readArgument(js::Value value, const char *function, int argument, T *out) {
    js::detail::Where where = { function, argument, nullptr };
    return js::Convert<T>::from(js::context(), value.get(), *out, where);
}

// ### Segments #################################################################

/**
 * Records an error of kind 'Kind' about segment 'index': "spi:
 * segments[2]: <what>" when the segments came from a list ('listed'), else
 * "spi: <what>" (a shortcut such as write()). Returns false.
 */
template<class Kind>
bool failSegment(bool listed, size_t index, const char *what) {
    if (listed) {
        js::fail<Kind>("spi: segments[%u]: %s", (unsigned)index, what);
    } else {
        js::fail<Kind>("spi: %s", what);
    }

    return false;
}

/**
 * A draft that sends or exchanges 'bytes' (a new reference is taken).
 */
Draft bytesDraft(Kind kind, js::Value bytes, int dc) {
    return Draft{ kind, bytes.dup(), 0, 0, dc };
}

/**
 * A draft that reads 'size' bytes.
 */
Draft readDraft(size_t size) {
    return Draft{ Kind::read, JS_UNDEFINED, size, 0, 1 };
}

/**
 * Frees the JS values the drafts hold (not the array itself).
 */
void releaseDraftSources(Draft *drafts, size_t count) {
    JSContext *ctx = js::context();

    for (size_t i = 0; i < count; i++) {
        JS_FreeValue(ctx, drafts[i].source);
        drafts[i].source = JS_UNDEFINED;
    }
}

/**
 * Fills *draft from one converted item of a transaction list. False with a
 * TypeError when it is not exactly one kind, or gives 'dc' to a device
 * without a DC pin.
 */
bool draftOf(const SegmentOptions *options, bool hasDc, size_t index, Draft *draft) {
    // exactly one kind
    bool isWrite = !options->write.isUndefined();
    bool isRead = options->read.has();
    bool isTransfer = !options->transfer.isUndefined();
    bool isDelay = options->delay.has();
    int kinds = (int)isWrite + (int)isRead + (int)isTransfer + (int)isDelay;
    if (kinds != 1) {
        return failSegment<js::TypeError>(true, index, "a segment is exactly one of { write }, { read }, { transfer }, { delay }");
    }

    // a DC level only for a device that has the pin
    if (options->dc.has() && !hasDc) {
        return failSegment<js::TypeError>(true, index, "dc given, but the device has no dc pin");
    }

    int dc = options->dc.has() ? (int)options->dc.value() : 1;

    // the draft of its kind
    if (isWrite) {
        *draft = bytesDraft(Kind::write, options->write, dc);
    } else if (isTransfer) {
        *draft = bytesDraft(Kind::transfer, options->transfer, dc);
    } else if (isRead) {
        *draft = readDraft((size_t)(int)options->read.value());
        draft->dc = dc;
    } else {
        *draft = Draft{ Kind::delay, JS_UNDEFINED, 0, (uint32_t)(int)options->delay.value(), dc };
    }

    return true;
}

/**
 * Reads the list given to transaction() into a new array of drafts
 * (memory::allocate, freed by the caller with its sources). False with the
 * error recorded: not an array, 0 or too many items, an invalid item.
 */
bool readDrafts(js::Value list, bool hasDc, Draft **out, size_t *count) {
    // an array of 1 to MAX_SEGMENTS items
    JSContext *ctx = js::context();
    if (!JS_IsArray(ctx, list.get())) {
        js::fail<js::TypeError>("spi: segments must be an array");
        return false;
    }

    int64_t length = 0;
    JSValue lengthValue = JS_GetPropertyStr(ctx, list.get(), "length");
    bool hasLength = JS_ToInt64(ctx, &length, lengthValue) == 0;
    JS_FreeValue(ctx, lengthValue);
    bool lengthValid = hasLength && length >= 1 && length <= MAX_SEGMENTS;
    if (!lengthValid) {
        js::fail<js::RangeError>("spi: a transaction has 1 to %d segments", MAX_SEGMENTS);
        return false;
    }

    // the drafts, empty until read
    size_t size = (size_t)length;
    Draft *drafts = (Draft *)memory::allocate(sizeof(Draft) * size);

    for (size_t i = 0; i < size; i++) {
        drafts[i] = Draft{ Kind::delay, JS_UNDEFINED, 0, 0, 1 };
    }

    // each item, converted as SegmentOptions (unknown fields refused), then
    // checked; errors name the item
    bool ok = true;

    for (size_t i = 0; i < size && ok; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, list.get(), (uint32_t)i);
        if (JS_IsException(item)) {
            js::detail::markPendingException();
            ok = false;
            break;
        }

        char place[32];
        snprintf(place, sizeof(place), "spi: segments[%u]", (unsigned)i);
        js::detail::Where where = { place, 0, nullptr };
        SegmentOptions options;
        ok = js::Convert<SegmentOptions>::from(ctx, item, options, where);
        JS_FreeValue(ctx, item);
        if (ok) {
            ok = draftOf(&options, hasDc, i, &drafts[i]);
        }
    }

    // refused: nothing kept
    if (!ok) {
        releaseDraftSources(drafts, size);
        memory::release(drafts);
        return false;
    }

    *out = drafts;
    *count = size;
    return true;
}

/**
 * Turns an array of numbers 0..255 into a new Uint8Array in *out (new
 * reference). False with the error recorded: empty or too long (RangeError),
 * an item that is not a number (TypeError) or not an integer 0..255
 * (RangeError).
 */
bool numbersToBytes(JSValueConst array, bool listed, size_t index, JSValue *out) {
    // 1 to MAX_SEGMENT_BYTES items
    JSContext *ctx = js::context();
    int64_t length = 0;
    JSValue lengthValue = JS_GetPropertyStr(ctx, array, "length");
    bool hasLength = JS_ToInt64(ctx, &length, lengthValue) == 0;
    JS_FreeValue(ctx, lengthValue);
    if (!hasLength || length < 1) {
        return failSegment<js::RangeError>(listed, index, "no bytes (an empty segment)");
    }

    if (length > MAX_SEGMENT_BYTES) {
        return failSegment<js::RangeError>(listed, index, "too many bytes (1048576 at most)");
    }

    // the Uint8Array
    js::Buffer bytes = js::Buffer::create((size_t)length);
    if (bytes.data() == nullptr) {
        return false;
    }

    // each item, a number that is an integer 0..255
    for (int64_t i = 0; i < length; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, array, (uint32_t)i);
        double number = 0;
        bool isNumber = JS_IsNumber(item) && JS_ToFloat64(ctx, &number, item) >= 0;
        JS_FreeValue(ctx, item);
        if (!isNumber) {
            return failSegment<js::TypeError>(listed, index, "bytes must be a Uint8Array, another typed array, an ArrayBuffer or an array of numbers");
        }

        bool isByte = number == floor(number) && number >= 0 && number <= 255;
        if (!isByte) {
            return failSegment<js::RangeError>(listed, index, "a byte is an integer 0..255");
        }

        bytes.data()[i] = (uint8_t)number;
    }

    *out = bytes.value().dup();
    return true;
}

/**
 * Pass that may run JS (getters of the items): every source given as an
 * array of numbers becomes a Uint8Array, so that the bytes of every source
 * can then be viewed without running JS. False with the error recorded.
 */
bool convertNumberArrays(Draft *drafts, size_t count, bool listed) {
    JSContext *ctx = js::context();

    for (size_t i = 0; i < count; i++) {
        // only the sources given as arrays
        if (!JS_IsArray(ctx, drafts[i].source)) {
            continue;
        }

        // replaced by their Uint8Array
        JSValue bytes = JS_UNDEFINED;
        if (!numbersToBytes(drafts[i].source, listed, i, &bytes)) {
            return false;
        }

        JS_FreeValue(ctx, drafts[i].source);
        drafts[i].source = bytes;
    }

    return true;
}

/**
 * The bytes of the source of draft 'index' (a typed array or an
 * ArrayBuffer, after convertNumberArrays), valid while the draft holds its
 * source and no JS runs. False with the error recorded.
 */
bool viewBytes(const Draft *draft, bool listed, size_t index, const uint8_t **data, size_t *size) {
    // a typed array or an ArrayBuffer; the draft keeps it alive
    JSContext *ctx = js::context();
    uint8_t *bytes = nullptr;
    JSValue buffer = JS_UNDEFINED;
    if (!js::detail::bytesOf(ctx, draft->source, &bytes, size, &buffer)) {
        return failSegment<js::TypeError>(listed, index, "bytes must be a Uint8Array, another typed array, an ArrayBuffer or an array of numbers 0..255");
    }

    JS_FreeValue(ctx, buffer);

    // 1 to MAX_SEGMENT_BYTES bytes
    if (*size == 0) {
        return failSegment<js::RangeError>(listed, index, "no bytes (an empty segment)");
    }

    if (*size > (size_t)MAX_SEGMENT_BYTES) {
        return failSegment<js::RangeError>(listed, index, "too many bytes (1048576 at most)");
    }

    *data = bytes;
    return true;
}

/**
 * Frees segments made by copySegments() and the copies they point to.
 */
void releaseSegments(hal::spi::Segment *segments, size_t count) {
    // nothing made
    if (segments == nullptr) {
        return;
    }

    // the copies (a made segment: what its maker needs and its piece), then
    // the array
    for (size_t i = 0; i < count; i++) {
        memory::release((void *)segments[i].write);
        memory::release(segments[i].read);
        memory::release(segments[i].makeOpaque);
        memory::release(segments[i].piece);
    }

    memory::release(segments);
}

/**
 * The hal segments of the drafts for a queued transaction: bytes copied,
 * read buffers allocated (memory::allocate; freed with releaseSegments).
 * Runs no JS. False with the error recorded (nothing kept).
 */
bool copySegments(const Draft *drafts, size_t count, bool listed, hal::spi::Segment **out) {
    // every segment empty first: a failure frees what was made
    hal::spi::Segment *segments = (hal::spi::Segment *)memory::allocate(sizeof(hal::spi::Segment) * count);

    for (size_t i = 0; i < count; i++) {
        segments[i] = hal::spi::Segment{ nullptr, nullptr, 0, drafts[i].dc, 0, nullptr, nullptr, nullptr, 0 };
    }

    // each segment from its draft
    bool ok = true;

    for (size_t i = 0; i < count && ok; i++) {
        const Draft *draft = &drafts[i];
        hal::spi::Segment *segment = &segments[i];
        bool sends = draft->kind == Kind::write || draft->kind == Kind::transfer;

        // the bytes sent, copied; a transfer receives as many
        if (sends) {
            const uint8_t *data = nullptr;
            size_t size = 0;
            ok = viewBytes(draft, listed, i, &data, &size);
            if (ok) {
                uint8_t *copy = (uint8_t *)memory::allocate(size);
                memcpy(copy, data, size);
                segment->write = copy;
                segment->size = size;
            }

            if (ok && draft->kind == Kind::transfer) {
                segment->read = (uint8_t *)memory::allocate(size);
            }
        }

        // a read: its buffer
        if (draft->kind == Kind::read) {
            segment->read = (uint8_t *)memory::allocate(draft->readSize);
            segment->size = draft->readSize;
        }

        // a delay: its time
        if (draft->kind == Kind::delay) {
            segment->delayUs = draft->delayUs;
        }
    }

    // refused: nothing kept
    if (!ok) {
        releaseSegments(segments, count);
        return false;
    }

    *out = segments;
    return true;
}

/**
 * Fills 'segments' (count items) for a Sync call: the bytes sent in place
 * (valid while the drafts hold their sources and no JS runs), each read or
 * transfer into a new Uint8Array appended to the JS array 'results'. Runs
 * no user JS. False with the error recorded.
 */
bool viewSegments(const Draft *drafts, size_t count, bool listed, hal::spi::Segment *segments, JSValueConst results) {
    JSContext *ctx = js::context();
    uint32_t resultCount = 0;

    for (size_t i = 0; i < count; i++) {
        const Draft *draft = &drafts[i];
        hal::spi::Segment *segment = &segments[i];
        *segment = hal::spi::Segment{ nullptr, nullptr, 0, draft->dc, 0, nullptr, nullptr, nullptr, 0 };
        bool sends = draft->kind == Kind::write || draft->kind == Kind::transfer;
        bool receives = draft->kind == Kind::read || draft->kind == Kind::transfer;

        // the bytes sent, in place
        if (sends) {
            const uint8_t *data = nullptr;
            if (!viewBytes(draft, listed, i, &data, &segment->size)) {
                return false;
            }

            segment->write = data;
        }

        // the size of a read
        if (draft->kind == Kind::read) {
            segment->size = draft->readSize;
        }

        // what it receives goes straight into a new Uint8Array of the results
        if (receives) {
            js::Buffer received = js::Buffer::create(segment->size);
            if (received.data() == nullptr) {
                return false;
            }

            segment->read = received.data();
            JS_SetPropertyUint32(ctx, results, resultCount, received.value().dup());
            resultCount++;
        }

        // a delay: its time
        if (draft->kind == Kind::delay) {
            segment->delayUs = draft->delayUs;
        }
    }

    return true;
}

/**
 * Frees a job and what it owns.
 */
void releaseJob(Job *job) {
    releaseSegments(job->segments, job->count);
    memory::release(job);
}

class Spi;

// The open Spi object of each port and its generation (JS thread): a device
// finds its bus through them, never through a pointer it keeps (the
// finalizers of a run may come in any order).
Spi *busOwners[MAX_PORTS] = {};             // nullptr: port closed
uint32_t busGenerations[MAX_PORTS] = {};    // generation of the owner
uint32_t lastGeneration = 0;                // given to the last bus opened

// ### Spi ######################################################################

/**
 * An SPI master bus (JS class Spi).
 */
class Spi {
    JS_CLASS(Spi);

public:
    /**
     * Finalizer (no JS here): waits for the started transaction, drops the
     * queued ones, frees the devices and the bus (the promises still
     * pending are dropped with the run).
     */
    ~Spi() {
        closeBus();
    }

    /**
     * JS constructor: new Spi(port, { sclk, mosi, miso, maxTransfer }) takes
     * controller 'port' of the chip. Throws if it is taken or refused.
     */
    void init(StrictInt<0, 255> port, BusOptions options) {
        // the clock and at least one data pin
        if (!options.sclk.has()) {
            return (void)js::fail<js::TypeError>("spi: the option sclk is required");
        }

        bool hasData = options.mosi.has() || options.miso.has();
        if (!hasData) {
            return (void)js::fail<js::TypeError>("spi: at least one of the options mosi and miso is required");
        }

        // a port this module can hold, not open here already
        int number = (int)port;
        char what[32];
        snprintf(what, sizeof(what), "spi port %d", number);
        if (number >= MAX_PORTS) {
            return (void)js::failSystem(ENOENT, what);
        }

        if (busOwners[number] != nullptr) {
            return (void)js::failSystem(EBUSY, what);
        }

        // the bus in the hal
        int sclk = (int)options.sclk.value();
        int mosi = options.mosi.has() ? (int)options.mosi.value() : -1;
        int miso = options.miso.has() ? (int)options.miso.value() : -1;
        hal::spi::Status status = hal::spi::openBus(number, sclk, mosi, miso, (size_t)(int)options.maxTransfer);
        if (status != hal::spi::Status::ok) {
            return (void)js::failSystem(errorOf(status), what);
        }

        // the ends of its started transactions, through the loop
        _port = number;
        _loop = rt::Loop::current();
        _source = _loop->addSource(&onMessage, this);
        busOwners[number] = this;
        busGenerations[number] = ++lastGeneration;

        doneLock.lock();
        completions[number] = Completion{ _loop->mailbox(), _source, 0, true, hal::spi::Status::ok };
        completions[number].mailbox->retain();
        doneLock.unlock();
    }

    /**
     * JS: a device on this bus, with its chip select, clock, mode, bit order
     * and DC pin. At most MAX_DEVICES per bus.
     */
    JS_EXPORT(device) js::Value device(js::Optional<DeviceOptions> options);

    /**
     * JS: closes the bus and its devices, after the started transaction
     * (its promise settled with its result); the queued ones are rejected
     * (ECANCELED). The object can no longer be used.
     */
    JS_EXPORT(close) void close() {
        // closed already: nothing to do
        if (_port < 0) {
            return;
        }

        // the started transaction ends, then the queued ones are cancelled
        if (_running != nullptr) {
            completeRunning(waitRunning());
        }

        cancelJobs(-1);

        // the devices, the bus and its source
        closeBus();
        updateKeepAlive();
    }

    /**
     * True while the bus is open; else false with an error pending (EBADF).
     */
    bool checkOpen() {
        if (_port < 0) {
            js::failSystem(EBADF, "spi bus closed");
            return false;
        }

        return true;
    }

    /**
     * Queues a transaction of 'device' and returns its promise, settled
     * with 'answer'. 'segments' is taken (freed on every path). EAGAIN when
     * MAX_PENDING promises are pending already.
     */
    js::Promise<js::Value> queue(int device, hal::spi::Segment *segments, size_t count, Answer answer) {
        // room for one more promise
        int slot = freeSlot();
        if (slot < 0) {
            releaseSegments(segments, count);
            char what[64];
            snprintf(what, sizeof(what), "spi port %d: %d transactions pending", _port, MAX_PENDING);
            return js::failSystem(EAGAIN, what);
        }

        // its promise first: a job never runs without one
        js::Promise<js::Value> promise = js::Promise<js::Value>::create();
        if (!promise.resolver().pending()) {
            releaseSegments(segments, count);
            return promise;
        }

        // the job, at the end of the queue
        Job *job = (Job *)memory::allocate(sizeof(Job));
        uint32_t id = ++_lastId;
        *job = Job{ id, device, segments, count, nullptr };
        if (_last == nullptr) {
            _first = job;
        } else {
            _last->next = job;
        }

        _last = job;
        _pending[slot].id = id;
        _pending[slot].answer = answer;
        _pending[slot].resolver = promise.resolver();

        // started at once if the bus is idle; the program is kept alive
        startNext();
        updateKeepAlive();
        return promise;
    }

    /**
     * JS thread, before a Sync call: empties the bus. Waits for the started
     * transaction, runs the queued ones here in order, settling their
     * promises; again while settling queued more (a getter may).
     */
    void drain() {
        while (_running != nullptr || _first != nullptr) {
            // the started one
            if (_running != nullptr) {
                completeRunning(waitRunning());
            }

            // the queued ones, on this thread
            while (_first != nullptr) {
                Job *job = popJob();
                hal::spi::Status status = hal::spi::run(job->device, job->segments, job->count);
                settleJob(job, status);
                releaseJob(job);
            }
        }

        // the devices closed meanwhile go, the program may end
        removeClosedDevices();
        updateKeepAlive();
    }

    /**
     * Records the error of 'status' for the current call, naming the port.
     * Returns the failure.
     */
    js::Failure failWith(hal::spi::Status status) {
        char what[32];
        snprintf(what, sizeof(what), "spi port %d", _port);
        return js::failSystem(errorOf(status), what);
    }

    /**
     * A device of this bus is closed (its hal number): its queued
     * transactions are rejected with ECANCELED when 'cancel' (a close() from
     * JS; a finalizer runs no JS: they run), then it leaves the hal once
     * none of its transactions is left and the bus is idle.
     */
    void closeDevice(int device, bool cancel) {
        // marked: removed as soon as possible
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (_devices[i].number == device) {
                _devices[i].closing = true;
            }
        }

        // its queued transactions, cancelled when JS may run
        if (cancel) {
            cancelJobs(device);
        }

        removeClosedDevices();
        if (cancel) {
            updateKeepAlive();
        }
    }

    /**
     * The port; -1 when closed.
     */
    int port() const {
        return _port;
    }

private:
    // A promise of a queued transaction, not settled yet.
    struct Pending {
        uint32_t                id = 0;                       // its job; 0: free slot
        Answer                  answer = Answer::nothing;     // what it resolves with
        js::Resolver<js::Value> resolver;                     // the promise; not pending: free
    };

    // A device added to the hal on this bus.
    struct DeviceSlot {
        int                     number = -1;                  // hal device; -1: free slot
        bool                    closing = false;              // closed: removed once it has no transaction left
    };

    /**
     * A free slot of _pending; -1 if none.
     */
    int freeSlot() {
        for (int i = 0; i < MAX_PENDING; i++) {
            if (!_pending[i].resolver.pending()) {
                return i;
            }
        }

        return -1;
    }

    /**
     * The pending promise of job 'id'; nullptr if none (cancelled, or the
     * bus closed).
     */
    Pending *pendingOf(uint32_t id) {
        for (int i = 0; i < MAX_PENDING; i++) {
            if (_pending[i].resolver.pending() && _pending[i].id == id) {
                return &_pending[i];
            }
        }

        return nullptr;
    }

    /**
     * Unlinks the oldest queued job and returns it (the queue is not empty).
     */
    Job *popJob() {
        Job *job = _first;
        _first = job->next;
        if (_first == nullptr) {
            _last = nullptr;
        }

        job->next = nullptr;
        return job;
    }

    /**
     * Starts the oldest queued job in the hal if no transaction runs; its
     * end comes to onTransactionDone().
     */
    void startNext() {
        // a transaction runs, or none waits
        if (_running != nullptr || _first == nullptr) {
            return;
        }

        // the job is the started one, awaited by its completion
        Job *job = popJob();
        _running = job;
        doneLock.lock();
        completions[_port].jobId = job->id;
        completions[_port].finished = false;
        doneLock.unlock();

        // started; 'done' may come before start() returns
        hal::spi::start(job->device, job->segments, job->count, &onTransactionDone, (void *)(intptr_t)_port);
    }

    /**
     * JS thread: waits, without the loop, for the end of the started
     * transaction and returns its status.
     */
    hal::spi::Status waitRunning() {
        doneLock.lock();

        while (!completions[_port].finished) {
            doneLock.wait(-1);
        }

        hal::spi::Status status = completions[_port].status;
        doneLock.unlock();
        return status;
    }

    /**
     * The started transaction is over with 'status': the hal finishes it
     * (before its read bytes are used), its promise is settled, the devices
     * closed meanwhile may go. Does not start the next one.
     */
    void completeRunning(hal::spi::Status status) {
        // no transaction started any more
        Job *job = _running;
        _running = nullptr;
        doneLock.lock();
        completions[_port].jobId = 0;
        doneLock.unlock();

        // the hal frees what start() took, then the promise gets the result
        hal::spi::finish(job->device);
        settleJob(job, status);
        releaseJob(job);
        removeClosedDevices();
    }

    /**
     * Settles the promise of 'job' (if still pending) from its status and
     * its read bytes.
     */
    void settleJob(Job *job, hal::spi::Status status) {
        // its promise
        Pending *pending = pendingOf(job->id);
        if (pending == nullptr) {
            return;
        }

        // a failure
        if (status != hal::spi::Status::ok) {
            pending->resolver.rejectSystem(errorOf(status));
            return;
        }

        // nothing to give
        if (pending->answer == Answer::nothing) {
            pending->resolver.resolve(js::Value());
            return;
        }

        // the bytes of each reading segment, in order
        JSContext *ctx = js::context();
        JSValue list = JS_NewArray(ctx);
        uint32_t listCount = 0;

        for (size_t i = 0; i < job->count; i++) {
            const hal::spi::Segment *segment = &job->segments[i];
            if (segment->read == nullptr) {
                continue;
            }

            js::Buffer received = js::Buffer::copy(segment->read, segment->size);
            JS_SetPropertyUint32(ctx, list, listCount, received.value().dup());
            listCount++;
        }

        // the list, or its only item
        js::Value result(ctx, list);
        if (pending->answer == Answer::bytes) {
            result = js::Value(ctx, JS_GetPropertyUint32(ctx, list, 0));
        }

        pending->resolver.resolve(result);
    }

    /**
     * Drops the queued jobs of hal device 'device' (-1: every device) and
     * rejects their promises with ECANCELED. The started one is not
     * touched.
     */
    void cancelJobs(int device) {
        // unlink the jobs to drop, keep the others in order
        Job *kept = nullptr;
        Job *keptLast = nullptr;
        Job *job = _first;

        while (job != nullptr) {
            Job *next = job->next;
            job->next = nullptr;
            bool dropped = device < 0 || job->device == device;
            if (dropped) {
                Pending *pending = pendingOf(job->id);
                if (pending != nullptr) {
                    pending->resolver.rejectSystem(ECANCELED);
                }

                releaseJob(job);
            } else if (keptLast == nullptr) {
                kept = job;
                keptLast = job;
            } else {
                keptLast->next = job;
                keptLast = job;
            }

            job = next;
        }

        _first = kept;
        _last = keptLast;
    }

    /**
     * True if a job of hal device 'device' is queued or started.
     */
    bool deviceHasJobs(int device) {
        // the started one, or a queued one
        if (_running != nullptr && _running->device == device) {
            return true;
        }

        for (Job *job = _first; job != nullptr; job = job->next) {
            if (job->device == device) {
                return true;
            }
        }

        return false;
    }

    /**
     * Removes from the hal the closed devices that have no transaction left,
     * when the bus is idle (the hal needs it).
     */
    void removeClosedDevices() {
        // a transaction runs: later, at its end
        if (_running != nullptr) {
            return;
        }

        // each closed device without jobs
        for (int i = 0; i < MAX_DEVICES; i++) {
            DeviceSlot *slot = &_devices[i];
            bool removable = slot->number >= 0 && slot->closing && !deviceHasJobs(slot->number);
            if (removable) {
                hal::spi::removeDevice(slot->number);
                *slot = DeviceSlot();
            }
        }
    }

    /**
     * Waits for the started transaction, drops the queued ones, frees the
     * devices and the bus, forgets the source (no JS: also called by the
     * finalizer; the promises are not settled). Nothing once done.
     */
    void closeBus() {
        // closed already: nothing to do
        if (_port < 0) {
            return;
        }

        // the started transaction ends; the queued ones are dropped
        if (_running != nullptr) {
            waitRunning();
            hal::spi::finish(_running->device);
            releaseJob(_running);
            _running = nullptr;
        }

        while (_first != nullptr) {
            releaseJob(popJob());
        }

        // every device, then the bus
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (_devices[i].number >= 0) {
                hal::spi::removeDevice(_devices[i].number);
                _devices[i] = DeviceSlot();
            }
        }

        hal::spi::closeBus(_port);

        // no more ends for this port (released outside the lock)
        doneLock.lock();
        rt::Mailbox *mailbox = completions[_port].mailbox;
        completions[_port] = Completion{ nullptr, { -1, 0 }, 0, true, hal::spi::Status::ok };
        doneLock.unlock();
        if (mailbox != nullptr) {
            mailbox->release();
        }

        // its source, and the port free for another object
        _loop->removeSource(_source);
        _loop = nullptr;
        busOwners[_port] = nullptr;
        _port = -1;
    }

    /**
     * Keeps the object (and the program) alive while a promise is pending.
     */
    void updateKeepAlive() {
        // kept while any promise is pending
        bool keep = false;

        for (int i = 0; i < MAX_PENDING && !keep; i++) {
            keep = _pending[i].resolver.pending();
        }

        if (keep != _kept) {
            js::keepAlive(this, keep);
            _kept = keep;
        }
    }

    /**
     * The started transaction is over (JS thread): settles its promise and
     * starts the next one. A Sync call may have completed it already.
     */
    static void onMessage(void *opaque, const rt::Message *message) {
        // the value: job id in the low 32 bits, hal status above
        Spi *bus = (Spi *)opaque;
        uint32_t id = (uint32_t)(message->value & 0xffffffff);
        hal::spi::Status status = (hal::spi::Status)(message->value >> 32);

        // still the started job: completed here
        bool isRunning = bus->_running != nullptr && bus->_running->id == id;
        if (isRunning) {
            bus->completeRunning(status);
        }

        // the next one, and the program may end once nothing is pending
        bus->startNext();
        bus->updateKeepAlive();
    }

    int                     _port = -1;                  // hal port; -1: closed (or not initialized)
    DeviceSlot              _devices[MAX_DEVICES];       // its devices in the hal
    Job                    *_first = nullptr;            // oldest queued job; nullptr: none (JS thread)
    Job                    *_last = nullptr;             // newest queued job
    Job                    *_running = nullptr;          // the job started in the hal; nullptr: idle
    Pending                 _pending[MAX_PENDING];       // promises not settled yet
    uint32_t                _lastId = 0;                 // id of the last job queued
    rt::Loop               *_loop = nullptr;             // loop of the run; nullptr: closed
    rt::SourceId            _source = { -1, 0 };         // message source of the ends of transactions
    bool                    _kept = false;               // js::keepAlive(this, true) is in force
};

// ### SpiDevice ################################################################

/**
 * A device on a bus (JS class SpiDevice), made by bus.device().
 */
class SpiDevice {
    JS_CLASS(SpiDevice);

public:
    /**
     * Finalizer (no JS here): the device leaves its bus once its queued
     * transactions are done.
     */
    ~SpiDevice() {
        closeDevice(false);
    }

    /**
     * JS constructor: refused, an SpiDevice comes from bus.device().
     */
    void init() {
        return (void)js::fail<js::TypeError>("spi: an SpiDevice comes from bus.device()");
    }

    /**
     * Ties the object to hal device 'device' of port 'port' (whose JS object
     * 'busValue' it keeps alive while it lives).
     */
    void bind(int port, js::Value busValue, int device, bool hasDc, uint32_t actualHz) {
        // its bus, known by number and generation, and kept alive
        _port = port;
        _busGeneration = busGenerations[port];
        _busValue = busValue;

        // the device in the hal, and what the hardware does
        _device = device;
        _hasDc = hasDc;
        frequency = (double)actualHz;
    }

    /**
     * JS: a transaction, the chip select held from the first segment to the
     * last; a promise of one Uint8Array per read / transfer segment.
     */
    JS_EXPORT(transaction) js::Value transaction(js::Value segments) {
        // the list read, then queued
        Draft *drafts = nullptr;
        size_t count = 0;
        js::Promise<js::Value> promise;
        if (ownerBus(true) != nullptr && readDrafts(segments, _hasDc, &drafts, &count)) {
            promise = queue(drafts, count, true, Answer::list);
            memory::release(drafts);
        }

        return promiseOrRejection(promise);
    }

    /**
     * JS: transaction() blocking the JS thread: the array of Uint8Arrays.
     */
    JS_EXPORT(transactionSync) js::Value transactionSync(js::Value segments) {
        // the list read, then run
        Draft *drafts = nullptr;
        size_t count = 0;
        if (ownerBus(true) == nullptr || !readDrafts(segments, _hasDc, &drafts, &count)) {
            return js::Value();
        }

        js::Value result = runSync(drafts, count, true, Answer::list);
        memory::release(drafts);
        return result;
    }

    /**
     * JS: writes bytes (DC high); a promise settled when done.
     */
    JS_EXPORT(write) js::Value write(js::Value bytes) {
        Draft draft = bytesDraft(Kind::write, bytes, 1);
        return promiseOrRejection(queue(&draft, 1, false, Answer::nothing));
    }

    /**
     * JS: write() blocking the JS thread.
     */
    JS_EXPORT(writeSync) void writeSync(js::Value bytes) {
        Draft draft = bytesDraft(Kind::write, bytes, 1);
        runSync(&draft, 1, false, Answer::nothing);
    }

    /**
     * JS: reads 'length' bytes, sending the fill byte; a promise of a
     * Uint8Array.
     */
    JS_EXPORT(read) js::Value read(js::Value length) {
        // the length, strictly an integer
        StrictInt<1, MAX_SEGMENT_BYTES> size;
        if (!readArgument(length, "SpiDevice.read", 1, &size)) {
            return promiseOrRejection(js::Promise<js::Value>());
        }

        // queued
        Draft draft = readDraft((size_t)(int)size);
        return promiseOrRejection(queue(&draft, 1, false, Answer::bytes));
    }

    /**
     * JS: read() blocking the JS thread: a Uint8Array.
     */
    JS_EXPORT(readSync) js::Value readSync(StrictInt<1, MAX_SEGMENT_BYTES> length) {
        Draft draft = readDraft((size_t)(int)length);
        return runSync(&draft, 1, false, Answer::bytes);
    }

    /**
     * JS: sends bytes and receives as many at the same time (full duplex); a
     * promise of the bytes received.
     */
    JS_EXPORT(transfer) js::Value transfer(js::Value bytes) {
        Draft draft = bytesDraft(Kind::transfer, bytes, 1);
        return promiseOrRejection(queue(&draft, 1, false, Answer::bytes));
    }

    /**
     * JS: transfer() blocking the JS thread: the bytes received.
     */
    JS_EXPORT(transferSync) js::Value transferSync(js::Value bytes) {
        Draft draft = bytesDraft(Kind::transfer, bytes, 1);
        return runSync(&draft, 1, false, Answer::bytes);
    }

    /**
     * JS: writes bytes then reads 'length' bytes under one chip select; a
     * promise of the bytes read.
     */
    JS_EXPORT(writeRead) js::Value writeRead(js::Value bytes, js::Value length) {
        // the length, strictly an integer
        StrictInt<1, MAX_SEGMENT_BYTES> size;
        if (!readArgument(length, "SpiDevice.writeRead", 2, &size)) {
            return promiseOrRejection(js::Promise<js::Value>());
        }

        // the two segments, queued
        Draft drafts[2] = { bytesDraft(Kind::write, bytes, 1), readDraft((size_t)(int)size) };
        return promiseOrRejection(queue(drafts, 2, false, Answer::bytes));
    }

    /**
     * JS: writeRead() blocking the JS thread: the bytes read.
     */
    JS_EXPORT(writeReadSync) js::Value writeReadSync(js::Value bytes, StrictInt<1, MAX_SEGMENT_BYTES> length) {
        Draft drafts[2] = { bytesDraft(Kind::write, bytes, 1), readDraft((size_t)(int)length) };
        return runSync(drafts, 2, false, Answer::bytes);
    }

    /**
     * JS: writes bytes with the DC pin low (a command of a display); a
     * promise settled when done. TypeError without a DC pin.
     */
    JS_EXPORT(command) js::Value command(js::Value bytes) {
        // a device with a DC pin (a closed one: EBADF below)
        if (!checkCommand()) {
            return promiseOrRejection(js::Promise<js::Value>());
        }

        // queued
        Draft draft = bytesDraft(Kind::write, bytes, 0);
        return promiseOrRejection(queue(&draft, 1, false, Answer::nothing));
    }

    /**
     * JS: command() blocking the JS thread.
     */
    JS_EXPORT(commandSync) void commandSync(js::Value bytes) {
        // a device with a DC pin
        if (!checkCommand()) {
            return;
        }

        // run
        Draft draft = bytesDraft(Kind::write, bytes, 0);
        runSync(&draft, 1, false, Answer::nothing);
    }

    /**
     * JS: closes the device: its queued transactions are rejected
     * (ECANCELED), the started one ends; it then leaves the bus. The object
     * can no longer be used.
     */
    JS_EXPORT(close) void close() {
        closeDevice(true);
    }

    // What the hardware does (set by bus.device(); writing it changes
    // nothing).
    /**
     * For the other native modules (spi_module.h, not seen by JS): queues a
     * transaction of hal segments made by the caller, which are taken (freed
     * on every path, as releaseSegments() does). Its promise, resolved with
     * undefined; rejected when the device or its bus is closed (EBADF) or
     * too many are pending (EAGAIN).
     */
    js::Value queueSegments(hal::spi::Segment *segments, size_t count) {
        // closed: nothing queued
        Spi *bus = ownerBus(true);
        if (bus == nullptr) {
            releaseSegments(segments, count);
            return promiseOrRejection(js::Promise<js::Value>());
        }

        return promiseOrRejection(bus->queue(_device, segments, count, Answer::nothing));
    }

    JS_EXPORT(frequency) double frequency = 0;   // clock, Hz, as the hardware makes it

private:
    /**
     * The bus object, if this device and its bus are still open: the owner
     * of the port, of the same generation. nullptr if not; with an error
     * recorded (EBADF) when 'report'.
     */
    Spi *ownerBus(bool report) {
        // open, and the bus still the one it was made on
        bool isOpen = _device >= 0 && busOwners[_port] != nullptr && busGenerations[_port] == _busGeneration;
        if (isOpen) {
            return busOwners[_port];
        }

        // closed
        if (report) {
            js::failSystem(EBADF, _device < 0 ? "spi device closed" : "spi bus closed");
        }

        return nullptr;
    }

    /**
     * True if command() may be called: the device is closed (the call then
     * fails with EBADF) or has a DC pin. Else false with a TypeError.
     */
    bool checkCommand() {
        bool usable = ownerBus(false) == nullptr || _hasDc;
        if (!usable) {
            js::fail<js::TypeError>("spi: command() needs a device with a dc pin");
        }

        return usable;
    }

    /**
     * Queues the transaction of 'drafts' on the bus; its promise, or an
     * empty one with the error recorded. The sources of the drafts are
     * released (not the array).
     */
    js::Promise<js::Value> queue(Draft *drafts, size_t count, bool listed, Answer answer) {
        // an open device, its numbers converted (JS may run: a getter)
        Spi *bus = ownerBus(true);
        bool converted = bus != nullptr && convertNumberArrays(drafts, count, listed);

        // still open after that, its bytes copied (no JS runs any more)
        bus = converted ? ownerBus(true) : nullptr;
        hal::spi::Segment *segments = nullptr;
        bool placed = bus != nullptr && copySegments(drafts, count, listed, &segments);
        releaseDraftSources(drafts, count);
        if (!placed) {
            return js::Promise<js::Value>();
        }

        // queued: the bus takes the segments
        return bus->queue(_device, segments, count, answer);
    }

    /**
     * Runs the transaction of 'drafts' on the JS thread, after the bus is
     * emptied, and returns 'answer' (undefined with the error recorded). The
     * sources of the drafts are released (not the array).
     *
     * Order: JS may run while the numbers are converted and while the
     * promises of the bus are settled, so the bytes are viewed only after
     * both, the device checked open again just before.
     */
    js::Value runSync(Draft *drafts, size_t count, bool listed, Answer answer) {
        // an open device, its numbers converted (JS may run: a getter)
        JSContext *ctx = js::context();
        Spi *bus = ownerBus(true);
        bool converted = bus != nullptr && convertNumberArrays(drafts, count, listed);

        // still open: the bus emptied (settling may run JS too), still open
        bus = converted ? ownerBus(true) : nullptr;
        if (bus != nullptr) {
            bus->drain();
            bus = ownerBus(true);
        }

        // the segments in place, the received bytes into the results
        hal::spi::Segment *segments = (hal::spi::Segment *)memory::allocate(sizeof(hal::spi::Segment) * count);
        JSValue results = JS_NewArray(ctx);
        bool placed = bus != nullptr && viewSegments(drafts, count, listed, segments, results);

        // run here
        hal::spi::Status status = hal::spi::Status::ok;
        if (placed) {
            status = hal::spi::run(_device, segments, count);
        }

        memory::release(segments);
        releaseDraftSources(drafts, count);

        // refused or failed: no result
        if (!placed || status != hal::spi::Status::ok) {
            JS_FreeValue(ctx, results);
            if (placed) {
                bus->failWith(status);
            }

            return js::Value();
        }

        // the answer
        js::Value list(ctx, results);
        switch (answer) {
            case Answer::nothing: {
                return js::Value();
            }
            case Answer::bytes: {
                return js::Value(ctx, JS_GetPropertyUint32(ctx, results, 0));
            }
            default: {
                return list;
            }
        }
    }

    /**
     * Closes the device in its bus if still open (a closed bus removed it
     * already); its queued transactions are cancelled when 'cancel' (from
     * JS). No JS otherwise: also called by the finalizer. Nothing once done.
     */
    void closeDevice(bool cancel) {
        Spi *bus = ownerBus(false);
        if (bus != nullptr) {
            bus->closeDevice(_device, cancel);
        }

        _device = -1;
    }

    int        _port = -1;             // port of its bus
    uint32_t   _busGeneration = 0;     // generation of the bus object it was made on
    js::Value  _busValue;              // the JS object of its bus, kept alive
    int        _device = -1;           // hal device; -1: closed (or not bound)
    bool       _hasDc = false;         // it has a DC pin (segments may give 'dc')
};

/**
 * JS: bus.device({ cs, frequency, mode, bitOrder, csActiveHigh, dc, fill }):
 * adds the device to the hal and returns its object. At most MAX_DEVICES per
 * bus (EBUSY).
 */
js::Value Spi::device(js::Optional<DeviceOptions> options) {
    // an open bus with room for a device
    if (!checkOpen()) {
        return js::Value();
    }

    int slot = -1;

    for (int i = 0; i < MAX_DEVICES && slot < 0; i++) {
        if (_devices[i].number < 0) {
            slot = i;
        }
    }

    char what[64];
    snprintf(what, sizeof(what), "spi port %d", _port);
    if (slot < 0) {
        return js::failSystem(EBUSY, what);
    }

    // its settings: the options, else the defaults
    DeviceOptions settings = options.has() ? options.value() : DeviceOptions();
    hal::spi::DeviceConfig config;
    config.cs = settings.cs.has() ? (int)settings.cs.value() : -1;
    config.dc = settings.dc.has() ? (int)settings.dc.value() : -1;
    config.frequencyHz = (uint32_t)(int)settings.frequency;
    config.mode = (int)settings.mode;
    config.lsbFirst = settings.bitOrder == BitOrder::lsb;
    config.csActiveHigh = settings.csActiveHigh;
    config.fill = (uint8_t)(int)settings.fill;

    // the device in the hal, counted by the bus
    int device = -1;
    uint32_t actualHz = 0;
    hal::spi::Status status = hal::spi::addDevice(_port, &config, &device, &actualHz);
    if (status != hal::spi::Status::ok) {
        return js::failSystem(errorOf(status), what);
    }

    _devices[slot].number = device;
    _devices[slot].closing = false;

    // its JS object, which keeps the bus object alive; none: the device
    // leaves the hal as a closed one (once the bus is idle)
    js::Created<SpiDevice> created = js::create<SpiDevice>();
    if (created.object == nullptr) {
        _devices[slot].closing = true;
        removeClosedDevices();
        return js::Value();
    }

    JSContext *ctx = js::context();
    js::Value self(ctx, js::Convert<Spi *>::to(ctx, this));
    created.object->bind(_port, self, device, config.dc >= 0, actualHz);
    return created.value;
}

} // namespace

js::Value device::queueSpiSegments(js::Value deviceValue, hal::spi::Segment *segments, size_t count) {
    // a device of this module, else a TypeError
    SpiDevice *target = nullptr;
    if (!readArgument(deviceValue, "spi", 1, &target)) {
        releaseSegments(segments, count);
        return js::Value();
    }

    return target->queueSegments(segments, count);
}

/**
 * The "device/spi" module: the classes Spi and SpiDevice.
 */
JS_MODULE(spi, module) {
    JS_ADD_CLASS(module, Spi);
    JS_ADD_CLASS(module, SpiDevice);
}
