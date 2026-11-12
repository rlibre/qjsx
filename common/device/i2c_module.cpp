/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "device/i2c": I2C master buses and the devices on them, through the
// hardware of the port (hal_i2c.h), on the boards whose port implements it
// (registered by the port, see device_modules.h).
//
//   import { I2c } from "device/i2c";
//   const bus = new I2c(0, { sda: 34, scl: 48, frequency: 400000 });
//   const sensor = bus.device(0x48, { frequency: 100000 });     // I2cDevice
//   await sensor.write([0x01, 0x60]);                           // array of numbers or Uint8Array
//   const bytes = await sensor.writeRead([0x00], 2);            // Uint8Array, after a repeated start
//   const now = sensor.readSync(2);                             // the same, blocking the JS thread
//   const there = await bus.probe(0x70);                        // true / false, never throws
//   bus.reset();                                                // frees a stuck bus
//   bus.close();                                                // closes its devices too
//
// Errors carry a Node-like 'code': ENXIO (nobody answered, NACK), ETIMEDOUT,
// EIO (anything else).
//
// Order: the transfers of a bus run one at a time, in the order of the calls.
// The promises go through one "i2c" task shared by every bus (it calls the
// blocking hal and posts the result to the loop of the run); a Sync call
// first waits until the transfers already queued for its bus are done, then
// runs on the JS thread. A bus with pending promises keeps the program
// running. A sequence that must not be cut by another transfer (select a
// channel of a switch, then read) is the program's business.
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "jsbind.h"
#include "hal_i2c.h"
#include "loop.h"
#include "memory.h"
#include "platform.h"

namespace {

// Options of 'new I2c(port, options)'.
struct BusOptions {
    js::Optional<js::Range<int, 0, 255>> sda;           // SDA pin (required)
    js::Optional<js::Range<int, 0, 255>> scl;           // SCL pin (required)
    js::Range<int, 1000, 5000000> frequency = 100000;   // default clock of its devices, Hz
    bool pullUp = false;                                // the chip's pull-ups (too weak for 400 kHz)
    js::Range<int, 1, 60000> timeout = 100;             // default time limit of a transfer, ms
};
JS_STRUCT(BusOptions, sda, scl, frequency, pullUp, timeout);

// Options of 'bus.device(address, options)'.
struct DeviceOptions {
    js::Optional<js::Range<int, 1000, 5000000>> frequency;   // clock, Hz; default: the bus's
    js::Optional<js::Range<int, 1, 60000>> timeout;          // ms; default: the bus's
};
JS_STRUCT(DeviceOptions, frequency, timeout);

const int MAX_BUSES = 8;               // buses this module can hold (the hal may have fewer)
const int MAX_DEVICES = 16;            // devices of a bus
const int MAX_PENDING = 16;            // promises of a bus not settled yet
const int MAX_TRANSFER = 4096;         // bytes written or read by one transfer
const int MESSAGE_DONE = 1;            // a queued transfer is over; value: job id | status << 32

// What a queued transfer resolves its promise with.
enum class Answer {
    nothing,                           // a write: undefined
    bytes,                             // a read: the bytes read
    presence,                          // a probe: true / false
};

// A transfer waiting for the "i2c" task (or running), owned by the queue.
struct Job {
    int           bus;                 // hal bus
    int           device;              // hal device; -1: a probe of 'address'
    int           address;             // the address probed
    uint8_t      *writeData;           // copy of the bytes to write (memory::allocate); nullptr: none
    size_t        writeSize;           // bytes to write
    size_t        readSize;            // bytes to read
    int           timeoutMs;           // time limit
    uint32_t      id;                  // number of the job in its bus object (finds its promise)
    rt::Mailbox  *mailbox;             // where the result goes, retained
    rt::SourceId  source;              // message source of the bus object
    Job          *next;                // next in the queue
};

// The queue of the "i2c" task, shared by every bus and every run.
Job *queueFirst = nullptr;             // oldest job; nullptr: empty (queueLock)
Job *queueLast = nullptr;              // newest job (queueLock)
int runningBus = -1;                   // bus of the job the task runs; -1: none (queueLock)
bool taskStarted = false;              // the task exists (queueLock)
platform::Signal queueLock;            // the queue; notified when a job is queued or ends

/**
 * The errno value of a status of the hal (0 for ok).
 */
int errorOf(hal::i2c::Status status) {
    switch (status) {
        case hal::i2c::Status::ok: {
            return 0;
        }
        case hal::i2c::Status::noAck: {
            return ENXIO;
        }
        case hal::i2c::Status::timeout: {
            return ETIMEDOUT;
        }
        default: {
            return EIO;
        }
    }
}

/**
 * Frees a job and what it owns.
 */
void releaseJob(Job *job) {
    job->mailbox->release();
    memory::release(job->writeData);
    memory::release(job);
}

/**
 * The "i2c" task: runs the queued jobs one by one, for good:
 * 1. takes the oldest job, marks its bus busy;
 * 2. runs it through the blocking hal, reading into a new block;
 * 3. posts the result (status, bytes read) to the loop of its run;
 * 4. marks the bus free and wakes whoever waits for it.
 */
void runJobs(void *) {
    queueLock.lock();
    for (;;) {
        // 1. the oldest job
        while (queueFirst == nullptr) {
            queueLock.wait(-1);
        }

        Job *job = queueFirst;
        queueFirst = job->next;
        if (queueFirst == nullptr) {
            queueLast = nullptr;
        }
        runningBus = job->bus;
        queueLock.unlock();

        // 2. run it
        uint8_t *readData = job->readSize > 0 ? (uint8_t *)memory::allocate(job->readSize) : nullptr;
        hal::i2c::Status status = hal::i2c::Status::failed;
        if (job->device < 0) {
            status = hal::i2c::probe(job->bus, job->address, job->timeoutMs);
        } else {
            status = hal::i2c::transfer(job->device, job->writeData, job->writeSize, readData, job->readSize,
                                        job->timeoutMs);
        }

        // 3. its result, with the bytes read when it succeeded
        int64_t value = (int64_t)job->id | ((int64_t)status << 32);
        if (status == hal::i2c::Status::ok && readData != nullptr) {
            job->mailbox->post(job->source, MESSAGE_DONE, readData, job->readSize, value);
        } else {
            memory::release(readData);
            job->mailbox->post(job->source, MESSAGE_DONE, nullptr, 0, value);
        }
        releaseJob(job);

        // 4. the bus is free
        queueLock.lock();
        runningBus = -1;
        queueLock.notify();
    }
}

/**
 * True if a job of 'bus' is queued or running. queueLock held.
 */
bool busHasJobs(int bus) {
    if (runningBus == bus) {
        return true;
    }
    for (Job *job = queueFirst; job != nullptr; job = job->next) {
        if (job->bus == bus) {
            return true;
        }
    }
    return false;
}

/**
 * JS thread: waits until no job of 'bus' is queued or running (before a
 * Sync call, a reset or a close).
 */
void waitBusIdle(int bus) {
    queueLock.lock();
    while (busHasJobs(bus)) {
        queueLock.wait(-1);
    }
    queueLock.unlock();
}

/**
 * JS thread: drops the queued jobs of 'bus' and waits for the one running
 * (close). The result of that one goes to a source already removed.
 */
void dropJobs(int bus) {
    queueLock.lock();

    // unlink the queued jobs of the bus
    Job *kept = nullptr;
    Job *keptLast = nullptr;
    Job *job = queueFirst;
    while (job != nullptr) {
        Job *next = job->next;
        if (job->bus == bus) {
            releaseJob(job);
        } else {
            job->next = nullptr;
            if (keptLast == nullptr) {
                kept = job;
            } else {
                keptLast->next = job;
            }
            keptLast = job;
        }
        job = next;
    }
    queueFirst = kept;
    queueLast = keptLast;

    // the running one
    while (runningBus == bus) {
        queueLock.wait(-1);
    }
    queueLock.unlock();
}

/**
 * JS thread: queues 'job' for the task (started at the first job). False if
 * the task cannot start (the job is then freed).
 */
bool queueJob(Job *job) {
    queueLock.lock();
    if (!taskStarted) {
        taskStarted = platform::startThread(&runJobs, nullptr, platform::defaultThreadOptions("i2c"));
    }
    if (!taskStarted) {
        queueLock.unlock();
        releaseJob(job);
        return false;
    }

    job->next = nullptr;
    if (queueLast == nullptr) {
        queueFirst = job;
    } else {
        queueLast->next = job;
    }
    queueLast = job;
    queueLock.notify();
    queueLock.unlock();
    return true;
}

/**
 * Copies the bytes of 'value' (a typed array, an ArrayBuffer or an array of
 * numbers 0..255) into a new block (memory::allocate) given in *out. False
 * with a TypeError pending if it is none of them, empty or longer than
 * MAX_TRANSFER.
 */
bool copyBytes(js::Value value, uint8_t **out, size_t *size) {
    JSContext *ctx = js::context();
    *out = nullptr;
    *size = 0;

    // a typed array or an ArrayBuffer: copied as it is
    uint8_t *data = nullptr;
    JSValue buffer = JS_UNDEFINED;
    if (!JS_IsArray(ctx, value.get()) && js::detail::bytesOf(ctx, value.get(), &data, size, &buffer)) {
        bool fits = *size > 0 && *size <= (size_t)MAX_TRANSFER;
        if (fits) {
            *out = (uint8_t *)memory::allocate(*size);
            memcpy(*out, data, *size);
        }
        JS_FreeValue(ctx, buffer);
        if (!fits) {
            js::fail<js::TypeError>("i2c: 1 to %d bytes", MAX_TRANSFER);
        }
        return fits;
    }

    // an array of numbers, each 0..255
    int64_t length = 0;
    bool valid = JS_IsArray(ctx, value.get());
    if (valid) {
        JSValue lengthValue = JS_GetPropertyStr(ctx, value.get(), "length");
        valid = JS_ToInt64(ctx, &length, lengthValue) == 0 && length > 0 && length <= MAX_TRANSFER;
        JS_FreeValue(ctx, lengthValue);
    }

    uint8_t *bytes = valid ? (uint8_t *)memory::allocate((size_t)length) : nullptr;
    for (int64_t i = 0; i < length && valid; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, value.get(), (uint32_t)i);
        int64_t byte = 0;
        valid = JS_IsNumber(item) && JS_ToInt64(ctx, &byte, item) == 0 && byte >= 0 && byte <= 255;
        JS_FreeValue(ctx, item);
        bytes[i] = (uint8_t)byte;
    }

    if (!valid) {
        memory::release(bytes);
        js::fail<js::TypeError>("i2c: bytes must be a Uint8Array or an array of 1 to %d numbers 0..255", MAX_TRANSFER);
        return false;
    }

    *out = bytes;
    *size = (size_t)length;
    return true;
}

class I2c;

// The open I2c object of each bus and its generation (JS thread): a device
// finds its bus through them, never through a pointer it keeps (the
// finalizers of a run may come in any order).
I2c *busOwners[MAX_BUSES] = {};        // nullptr: bus closed
uint32_t busGenerations[MAX_BUSES] = {};   // generation of the owner
uint32_t lastGeneration = 0;           // given to the last bus opened

/**
 * An I2C master bus (JS class I2c).
 */
class I2c {
    JS_CLASS(I2c);

public:
    /**
     * Finalizer (no JS here): drops the jobs of the bus, frees its devices
     * and the bus (the promises still pending are dropped with the run).
     */
    ~I2c() {
        closeBus();
    }

    /**
     * JS constructor: new I2c(port, { sda, scl, frequency, pullUp,
     * timeout }) takes bus 'port' of the chip. Throws if it is taken or
     * refused.
     */
    void init(js::Range<int, 0, MAX_BUSES - 1> port, BusOptions options) {
        // the pins are required, the bus must exist and be free
        if (!options.sda.has() || !options.scl.has()) {
            return (void)js::fail<js::TypeError>("i2c: the options sda and scl are required");
        }
        int bus = (int)port;
        if (bus >= hal::i2c::busCount()) {
            return (void)js::fail<js::RangeError>("i2c: bus %d does not exist (%d buses)", bus, hal::i2c::busCount());
        }
        if (busOwners[bus] != nullptr) {
            return (void)js::fail("i2c: bus %d is already open", bus);
        }

        // the bus
        int sda = (int)options.sda.value();
        int scl = (int)options.scl.value();
        if (!hal::i2c::openBus(bus, sda, scl, options.pullUp)) {
            return (void)js::fail("i2c: cannot open bus %d on sda %d, scl %d", bus, sda, scl);
        }

        // its defaults, and the results of its queued transfers through the loop
        _bus = bus;
        _frequency = (int)options.frequency;
        _timeoutMs = (int)options.timeout;
        _loop = rt::Loop::current();
        _source = _loop->addSource(&onMessage, this);
        busOwners[bus] = this;
        busGenerations[bus] = ++lastGeneration;
    }

    /**
     * JS: the device at 7-bit 'address' on this bus, with its own clock and
     * time limit (default: the bus's).
     */
    JS_EXPORT(device) js::Value device(js::Range<int, 0, 127> address, js::Optional<DeviceOptions> options);

    /**
     * JS: a promise of true if something answers at 'address', false if
     * not. Rejected only for another error (ETIMEDOUT, EIO).
     */
    JS_EXPORT(probe) js::Promise<js::Value> probe(js::Range<int, 0, 127> address) {
        return queue(-1, (int)address, nullptr, 0, 0, _timeoutMs, Answer::presence);
    }

    /**
     * JS: probe() blocking the JS thread: true or false.
     */
    JS_EXPORT(probeSync) bool probeSync(js::Range<int, 0, 127> address) {
        if (!checkOpen()) {
            return false;
        }

        waitBusIdle(_bus);
        hal::i2c::Status status = hal::i2c::probe(_bus, (int)address, _timeoutMs);
        if (status == hal::i2c::Status::ok || status == hal::i2c::Status::noAck) {
            return status == hal::i2c::Status::ok;
        }
        return failWith(status, (int)address);
    }

    /**
     * JS: frees a bus that a device holds stuck, after the transfers queued.
     */
    JS_EXPORT(reset) void reset() {
        if (!checkOpen()) {
            return;
        }

        waitBusIdle(_bus);
        hal::i2c::resetBus(_bus);
    }

    /**
     * JS: closes the bus and its devices; the pending promises are rejected.
     * The object can no longer be used.
     */
    JS_EXPORT(close) void close() {
        closeBus();

        for (int i = 0; i < MAX_PENDING; i++) {
            if (_pending[i].resolver.pending()) {
                _pending[i].resolver.reject("i2c: bus closed");
            }
        }
        updateKeepAlive();
    }

    /**
     * True while the bus is open; else false with an error pending.
     */
    bool checkOpen() {
        if (_bus < 0) {
            js::fail("i2c: bus closed");
            return false;
        }
        return true;
    }

    /**
     * Queues a transfer (device >= 0) or a probe (device -1, 'address') and
     * returns its promise. 'writeData' is taken (freed on every path).
     */
    js::Promise<js::Value> queue(int device, int address, uint8_t *writeData, size_t writeSize, size_t readSize,
                                 int timeoutMs, Answer answer) {
        // the bus must be open, with room for one more promise
        int slot = freeSlot();
        if (!checkOpen() || slot < 0) {
            memory::release(writeData);
            if (_bus >= 0) {
                js::fail("i2c: too many transfers pending (%d at most)", MAX_PENDING);
            }
            return js::Promise<js::Value>();
        }

        // the job, for the task
        Job *job = (Job *)memory::allocate(sizeof(Job));
        uint32_t id = ++_lastId;
        *job = Job{ _bus, device, address, writeData, writeSize, readSize, timeoutMs, id, _loop->mailbox(), _source,
                    nullptr };
        job->mailbox->retain();
        if (!queueJob(job)) {
            return js::fail("i2c: cannot start the i2c task");
        }

        // its promise
        js::Promise<js::Value> promise = js::Promise<js::Value>::create();
        _pending[slot].id = id;
        _pending[slot].answer = answer;
        _pending[slot].address = address;
        _pending[slot].resolver = promise.resolver();
        updateKeepAlive();
        return promise;
    }

    /**
     * Records the error of 'status' for the current call (ENXIO, ETIMEDOUT,
     * EIO), naming the bus and 'address'. Returns the failure.
     */
    js::Failure failWith(hal::i2c::Status status, int address) {
        char what[64];
        snprintf(what, sizeof(what), "i2c bus %d address 0x%02x", _bus, address);
        return js::failSystem(errorOf(status), what);
    }

    /**
     * A device of this bus was added (its hal number), or removed.
     */
    bool addDeviceNumber(int device) {
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (_devices[i] < 0) {
                _devices[i] = device;
                return true;
            }
        }
        return false;
    }

    void removeDeviceNumber(int device) {
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (_devices[i] == device) {
                _devices[i] = -1;
            }
        }
    }

    int busNumber() const {
        return _bus;
    }

    int frequency() const {
        return _frequency;
    }

    int timeoutMs() const {
        return _timeoutMs;
    }

private:
    // A promise of a queued transfer, not settled yet.
    struct Pending {
        uint32_t                id = 0;                  // its job; 0: free slot
        Answer                  answer = Answer::nothing;   // what it resolves with
        int                     address = 0;             // for the error message
        js::Resolver<js::Value> resolver;                // the promise; not pending: free
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
     * Drops the jobs, frees the devices and the bus, forgets the source (no
     * JS: also called by the finalizer). Nothing once done.
     */
    void closeBus() {
        if (_bus < 0) {
            return;
        }

        dropJobs(_bus);
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (_devices[i] >= 0) {
                hal::i2c::removeDevice(_devices[i]);
                _devices[i] = -1;
            }
        }
        hal::i2c::closeBus(_bus);

        _loop->removeSource(_source);
        _loop = nullptr;
        busOwners[_bus] = nullptr;
        _bus = -1;
    }

    /**
     * Keeps the object (and the program) alive while a promise is pending.
     */
    void updateKeepAlive() {
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
     * A queued transfer is over (JS thread): settles its promise.
     */
    static void onMessage(void *opaque, const rt::Message *message) {
        I2c *bus = (I2c *)opaque;
        uint32_t id = (uint32_t)(message->value & 0xffffffff);
        hal::i2c::Status status = (hal::i2c::Status)(message->value >> 32);

        // its promise
        Pending *pending = nullptr;
        for (int i = 0; i < MAX_PENDING && pending == nullptr; i++) {
            if (bus->_pending[i].resolver.pending() && bus->_pending[i].id == id) {
                pending = &bus->_pending[i];
            }
        }
        if (pending == nullptr) {
            return;
        }

        // a probe: true / false, an error only for something else
        if (pending->answer == Answer::presence && status != hal::i2c::Status::failed
            && status != hal::i2c::Status::timeout) {
            JSContext *ctx = js::context();
            pending->resolver.resolve(js::Value(ctx, JS_NewBool(ctx, status == hal::i2c::Status::ok)));
        } else if (status != hal::i2c::Status::ok) {
            pending->resolver.rejectSystem(errorOf(status));
        } else if (pending->answer == Answer::bytes) {
            js::Buffer bytes = js::Buffer::copy(message->data, message->size);
            pending->resolver.resolve(bytes.value());
        } else {
            pending->resolver.resolve(js::Value());
        }

        bus->updateKeepAlive();
    }

    int                     _bus = -1;                   // hal bus; -1: closed (or not initialized)
    int                     _frequency = 100000;         // default clock of its devices, Hz
    int                     _timeoutMs = 100;            // default time limit, ms
    int                     _devices[MAX_DEVICES] = { -1, -1, -1, -1, -1, -1, -1, -1,
                                                      -1, -1, -1, -1, -1, -1, -1, -1 };   // hal devices; -1: free
    Pending                 _pending[MAX_PENDING];       // promises not settled yet
    uint32_t                _lastId = 0;                 // id of the last job queued
    rt::Loop               *_loop = nullptr;            // loop of the run; nullptr: closed
    rt::SourceId            _source = { -1, 0 };         // message source of the results
    bool                    _kept = false;               // js::keepAlive(this, true) is in force
};

/**
 * A device on a bus (JS class I2cDevice), made by bus.device().
 */
class I2cDevice {
    JS_CLASS(I2cDevice);

public:
    /**
     * Finalizer (no JS here): frees the device if its bus is still open.
     */
    ~I2cDevice() {
        closeDevice();
    }

    /**
     * JS constructor: refused, an I2cDevice comes from bus.device().
     */
    void init() {
        return (void)js::fail<js::TypeError>("i2c: an I2cDevice comes from bus.device()");
    }

    /**
     * Ties the object to hal device 'device' at 'address' of bus 'bus' (whose
     * JS object 'busValue' it keeps alive while it lives).
     */
    void bind(int bus, js::Value busValue, int device, int address, int timeoutMs) {
        _bus = bus;
        _busGeneration = busGenerations[bus];
        _busValue = busValue;
        _device = device;
        _address = address;
        _timeoutMs = timeoutMs;
    }

    /**
     * JS: writes bytes; a promise settled when done.
     */
    JS_EXPORT(write) js::Promise<js::Value> write(js::Value bytes) {
        I2c *bus = openBus();
        uint8_t *data = nullptr;
        size_t size = 0;
        if (bus == nullptr || !copyBytes(bytes, &data, &size)) {
            return js::Promise<js::Value>();
        }
        return bus->queue(_device, _address, data, size, 0, _timeoutMs, Answer::nothing);
    }

    /**
     * JS: reads 'length' bytes; a promise of a Uint8Array.
     */
    JS_EXPORT(read) js::Promise<js::Value> read(js::Range<int, 1, MAX_TRANSFER> length) {
        I2c *bus = openBus();
        if (bus == nullptr) {
            return js::Promise<js::Value>();
        }
        return bus->queue(_device, _address, nullptr, 0, (size_t)(int)length, _timeoutMs, Answer::bytes);
    }

    /**
     * JS: writes bytes then reads 'length' bytes after a repeated start (a
     * register read); a promise of a Uint8Array.
     */
    JS_EXPORT(writeRead) js::Promise<js::Value> writeRead(js::Value bytes, js::Range<int, 1, MAX_TRANSFER> length) {
        I2c *bus = openBus();
        uint8_t *data = nullptr;
        size_t size = 0;
        if (bus == nullptr || !copyBytes(bytes, &data, &size)) {
            return js::Promise<js::Value>();
        }
        return bus->queue(_device, _address, data, size, (size_t)(int)length, _timeoutMs, Answer::bytes);
    }

    /**
     * JS: write() blocking the JS thread.
     */
    JS_EXPORT(writeSync) void writeSync(js::Value bytes) {
        I2c *bus = openBus();
        uint8_t *data = nullptr;
        size_t size = 0;
        if (bus == nullptr || !copyBytes(bytes, &data, &size)) {
            return;
        }

        waitBusIdle(_bus);
        hal::i2c::Status status = hal::i2c::transfer(_device, data, size, nullptr, 0, _timeoutMs);
        memory::release(data);
        if (status != hal::i2c::Status::ok) {
            return (void)bus->failWith(status, _address);
        }
    }

    /**
     * JS: read() blocking the JS thread: a Uint8Array.
     */
    JS_EXPORT(readSync) js::Value readSync(js::Range<int, 1, MAX_TRANSFER> length) {
        I2c *bus = openBus();
        if (bus == nullptr) {
            return js::Value();
        }
        return transferSync(bus, nullptr, 0, (size_t)(int)length);
    }

    /**
     * JS: writeRead() blocking the JS thread: a Uint8Array.
     */
    JS_EXPORT(writeReadSync) js::Value writeReadSync(js::Value bytes, js::Range<int, 1, MAX_TRANSFER> length) {
        I2c *bus = openBus();
        uint8_t *data = nullptr;
        size_t size = 0;
        if (bus == nullptr || !copyBytes(bytes, &data, &size)) {
            return js::Value();
        }

        js::Value result = transferSync(bus, data, size, (size_t)(int)length);
        memory::release(data);
        return result;
    }

    /**
     * JS: frees the device, after the transfers already queued on its bus.
     * The object can no longer be used.
     */
    JS_EXPORT(close) void close() {
        closeDevice();
    }

private:
    /**
     * The bus object, if this device and its bus are still open: the owner
     * of the bus number, of the same generation. nullptr if not (no error).
     */
    I2c *ownerBus() {
        if (_device < 0 || busOwners[_bus] == nullptr || busGenerations[_bus] != _busGeneration) {
            return nullptr;
        }
        return busOwners[_bus];
    }

    /**
     * ownerBus(), with an error pending when it is nullptr.
     */
    I2c *openBus() {
        I2c *bus = ownerBus();
        if (bus == nullptr) {
            js::fail(_device < 0 ? "i2c: device closed" : "i2c: bus closed");
        }
        return bus;
    }

    /**
     * A blocking transfer that reads 'readSize' bytes straight into a new
     * Uint8Array, after the transfers queued for the bus.
     */
    js::Value transferSync(I2c *bus, const uint8_t *writeData, size_t writeSize, size_t readSize) {
        js::Buffer bytes = js::Buffer::create(readSize);
        waitBusIdle(_bus);
        hal::i2c::Status status = hal::i2c::transfer(_device, writeData, writeSize, bytes.data(), readSize, _timeoutMs);
        if (status != hal::i2c::Status::ok) {
            return bus->failWith(status, _address);
        }
        return bytes.value();
    }

    /**
     * Frees the hal device if its bus is still open (a closed bus freed it
     * already). No JS: also called by the finalizer. Nothing once done.
     */
    void closeDevice() {
        I2c *bus = ownerBus();
        if (bus != nullptr) {
            waitBusIdle(_bus);
            hal::i2c::removeDevice(_device);
            bus->removeDeviceNumber(_device);
        }
        _device = -1;
    }

    int        _bus = -1;              // bus number
    uint32_t   _busGeneration = 0;     // generation of the bus object it was made on
    js::Value  _busValue;              // the JS object of its bus, kept alive
    int        _device = -1;           // hal device; -1: closed (or not bound)
    int        _address = 0;           // 7-bit address
    int        _timeoutMs = 0;         // time limit, ms
};

/**
 * JS: bus.device(address, { frequency, timeout }): adds the device to the
 * hal and returns its object. At most MAX_DEVICES per bus.
 */
js::Value I2c::device(js::Range<int, 0, 127> address, js::Optional<DeviceOptions> options) {
    if (!checkOpen()) {
        return js::Value();
    }

    // its clock and time limit: the options, else the bus's
    int frequency = _frequency;
    int timeoutMs = _timeoutMs;
    if (options.has() && options.value().frequency.has()) {
        frequency = (int)options.value().frequency.value();
    }
    if (options.has() && options.value().timeout.has()) {
        timeoutMs = (int)options.value().timeout.value();
    }

    // the device in the hal, then its object, which keeps this one alive
    int device = hal::i2c::addDevice(_bus, (int)address, (uint32_t)frequency);
    if (device < 0) {
        return js::fail("i2c: cannot add the device 0x%02x on bus %d", (int)address, _bus);
    }
    if (!addDeviceNumber(device)) {
        hal::i2c::removeDevice(device);
        return js::fail("i2c: at most %d devices per bus", MAX_DEVICES);
    }

    js::Created<I2cDevice> created = js::create<I2cDevice>();
    if (created.object == nullptr) {
        removeDeviceNumber(device);
        hal::i2c::removeDevice(device);
        return js::Value();
    }

    JSContext *ctx = js::context();
    js::Value self(ctx, js::Convert<I2c *>::to(ctx, this));
    created.object->bind(_bus, self, device, (int)address, timeoutMs);
    return created.value;
}

} // namespace

/**
 * The "device/i2c" module: the classes I2c and I2cDevice.
 */
JS_MODULE(i2c, module) {
    JS_ADD_CLASS(module, I2c);
    JS_ADD_CLASS(module, I2cDevice);
}
