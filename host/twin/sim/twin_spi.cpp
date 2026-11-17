/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// hal::spi of the digital twin (see twin.h): the controllers of the board
// description ("spi": { "ports", "reserved" }), on any pins of the board, as
// through the GPIO matrix of the ESP32. A transaction goes to the component
// on its chip select (twin::spiListener, twin::spiTransaction: the model of
// a part answers it). With no component there, nothing drives MISO: every
// byte read is 0xFF, as with its pull-up. A transaction lasts its time on
// the wire (user, 2026-11-17): its bytes at the clock of its device, plus its
// delay segments; the thread that runs it waits, once it is done, for what
// is left of that time (the time spent making the bytes and answering
// counts, as the ESP32 makes a band while the previous one goes out): a
// program paces itself as on the board (a full screen of 307,200 bytes takes
// 61 ms at 40 MHz). Times below a millisecond are added up per bus and
// waited for once they make a millisecond. The segments are not cut in
// pieces of maxTransfer bytes. The clock of a device is what the ESP32-S3
// makes: its APB clock (80 MHz) divided by a whole number. Every transaction
// is kept in a trace for the page (the last SPI_TRACE_RECORDS).
//
// A made segment (hal_spi.h: a display's pixels, sys/draw) is run as the
// ESP32 runs it: piece by piece, each piece of at most its pieceSize bytes
// made by its 'make' just before it "goes out" (refused when pieceSize is 0
// or above the bus's maxTransfer, as on the ESP32). Its bytes are gathered
// into one write segment of the whole size, in order, with the segment's DC
// level: that is what the component and the trace get (the trace keeps its
// first bytes only, as for any write).
//
// Threads: run() on the program's thread: it waits for the component's
// answer, ANSWER_TIMEOUT_MS at most (then the bytes read are 0xFF and the
// trace says "timeout"). start() hands the transaction to the "twin spi"
// thread, which runs it the same way, then calls 'done'. The module never
// runs two transactions of one bus at once: one job per bus at most. lock
// protects the tables, the jobs and the trace, and wakes that thread.
#include "hal_spi.h"
#include "memory.h"
#include "twin.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

namespace {

const int MAX_PINS = 64;                 // pins the twin can have, as twin.cpp
const int DEVICES = 16;                  // devices of every bus together
const uint32_t SOURCE_CLOCK_HZ = 80000000;   // the clock the controller divides (APB of the ESP32-S3)
const int ANSWER_TIMEOUT_MS = 1000;      // how long a transaction waits for its component

// The controllers of the board ("spi" of the description; written by spiRead).
int ports[twin::SPI_MAX_PORTS];          // "ports": those the program may open
int portCount = 0;                       // entries of ports
int reserved[twin::SPI_MAX_PORTS];       // "reserved": those kept for the flash (busy)
int reservedCount = 0;                   // entries of reserved

// A bus: a controller the program opened (lock), by its index in 'ports'.
struct Bus {
    bool   open;                         // openBus() took it
    int    sclk;                         // its clock pin
    int    mosi;                         // its data out pin; -1: not wired
    int    miso;                         // its data in pin; -1: not wired
    size_t maxTransfer;                  // bytes in one go (DMA): recorded only
    int64_t owedMicros;                  // wire time not waited for yet (under a millisecond)
};

// A device added on a bus (lock).
struct Device {
    bool                    used;        // addDevice() gave it
    int                     bus;         // index of its bus in 'ports'
    hal::spi::DeviceConfig  config;      // as the module gave it
    uint32_t                actualHz;    // the clock the hardware makes
};

// A transaction handed to the "twin spi" thread by start() (lock).
struct Job {
    bool                      pending;   // waiting for the thread
    int                       device;    // the device it runs on
    const hal::spi::Segment  *segments;  // owned by the module until finish()
    size_t                    count;     // entries of segments
    hal::spi::DoneCallback    done;      // called when it is over
    void                     *opaque;    // given to done
};

Bus buses[twin::SPI_MAX_PORTS];          // lock
Device devices[DEVICES];                 // lock
Job jobs[twin::SPI_MAX_PORTS];           // lock: one per bus at most
bool threadStarted = false;              // lock: the "twin spi" thread runs
bool pinUsed[MAX_PINS];                  // lock: what setSpi() was last told of each pin
platform::Signal lock;                   // everything above, the trace; wakes the thread

// The trace (lock).
twin::TraceRing<twin::SpiRecord, twin::SPI_TRACE_RECORDS> trace;

/**
 * The index of controller 'port' in 'list' ('count' entries), -1 if absent.
 */
int indexIn(const int *list, int count, int port) {
    for (int i = 0; i < count; i++) {
        if (list[i] == port) {
            return i;
        }
    }

    return -1;
}

/**
 * Whether 'pin' is a pin of the board.
 */
bool isBoardPin(int pin) {
    return pin >= 0 && pin < twin::pinCount();
}

/**
 * The clock the ESP32-S3 makes for 'hz': its source divided by the whole
 * number closest to the ratio (at least 1: never above the source).
 */
uint32_t actualClock(uint32_t hz) {
    uint32_t divider = (SOURCE_CLOCK_HZ + hz / 2) / hz;
    if (divider < 1) {
        divider = 1;
    }

    return SOURCE_CLOCK_HZ / divider;
}

/**
 * Whether 'pin' is one of the pins of 'bus'.
 */
bool isBusPin(const Bus *bus, int pin) {
    return pin == bus->sclk || pin == bus->mosi || pin == bus->miso;
}

/**
 * The page learns which pins the buses and devices use now: each pin whose
 * use changed is told (setSpi, outside the lock).
 */
void showPins() {
    // the pins used now: those of the open buses, the chip selects and DC
    // pins of the devices
    bool used[MAX_PINS] = {};
    lock.lock();

    for (const Bus &bus : buses) {
        if (bus.open) {
            int busPins[3] = { bus.sclk, bus.mosi, bus.miso };

            for (int pin : busPins) {
                if (isBoardPin(pin)) {
                    used[pin] = true;
                }
            }
        }
    }

    for (const Device &device : devices) {
        if (device.used) {
            int devicePins[2] = { device.config.cs, device.config.dc };

            for (int pin : devicePins) {
                if (isBoardPin(pin)) {
                    used[pin] = true;
                }
            }
        }
    }

    // the changes, noted
    bool changed[MAX_PINS] = {};

    for (int pin = 0; pin < MAX_PINS; pin++) {
        changed[pin] = used[pin] != pinUsed[pin];
        pinUsed[pin] = used[pin];
    }

    lock.unlock();

    // told, outside the lock
    for (int pin = 0; pin < MAX_PINS; pin++) {
        if (changed[pin]) {
            twin::setSpi(pin, used[pin]);
        }
    }
}

/**
 * Keeps a transaction in the trace: the device, then the first segments
 * with their first bytes.
 */
void record(const Device *device, const hal::spi::Segment *segments, size_t count, twin::SpiOutcome outcome) {
    lock.lock();

    // the device, its settings, how it went
    twin::SpiRecord *entry = trace.add();
    entry->timeUs = twin::scopeNowUs();
    entry->port = ports[device->bus];
    entry->cs = device->config.cs;
    entry->dc = device->config.dc;
    entry->frequencyHz = device->actualHz;
    entry->mode = (uint8_t)device->config.mode;
    entry->lsbFirst = device->config.lsbFirst;
    entry->outcome = (uint8_t)outcome;
    entry->segmentCount = (uint32_t)count;

    // the first segments, with their first bytes
    size_t kept = count < (size_t)twin::SPI_TRACE_SEGMENTS ? count : (size_t)twin::SPI_TRACE_SEGMENTS;

    for (size_t i = 0; i < kept; i++) {
        const hal::spi::Segment *segment = &segments[i];
        twin::SpiSegmentRecord *copy = &entry->segments[i];
        size_t bytesKept = segment->size < twin::SPI_TRACE_BYTES ? segment->size : twin::SPI_TRACE_BYTES;
        copy->kind = (uint8_t)twin::spiKindOf(segment);
        copy->dc = (uint8_t)(segment->dc != 0 ? 1 : 0);
        copy->size = (uint32_t)segment->size;
        copy->delayUs = segment->delayUs;
        if (segment->write != nullptr) {
            memcpy(copy->written, segment->write, bytesKept);
        }

        if (segment->read != nullptr) {
            memcpy(copy->read, segment->read, bytesKept);
        }
    }

    lock.unlock();
}

/**
 * Whether one of the 'count' segments is a made segment.
 */
bool hasMadeSegment(const hal::spi::Segment *segments, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (segments[i].make != nullptr) {
            return true;
        }
    }

    return false;
}

/**
 * A copy of the 'count' segments where each made segment is a write
 * segment of its whole size, its bytes made piece by piece as the ESP32
 * sends them (owned: freeMade()). nullptr when a made segment's pieceSize
 * is 0 or above 'maxTransfer' (the ESP32 refuses it too).
 */
hal::spi::Segment *makeSegments(const hal::spi::Segment *segments, size_t count, size_t maxTransfer) {
    // every made segment with pieces the bus can send
    for (size_t i = 0; i < count; i++) {
        const hal::spi::Segment *segment = &segments[i];
        bool piecesFit = segment->pieceSize > 0 && segment->pieceSize <= maxTransfer;
        if (segment->make != nullptr && !piecesFit) {
            return nullptr;
        }
    }

    // the copy, every segment as it is
    hal::spi::Segment *copy = (hal::spi::Segment *)memory::allocate(sizeof(hal::spi::Segment) * count);
    memcpy(copy, segments, sizeof(hal::spi::Segment) * count);

    for (size_t i = 0; i < count; i++) {
        // a made segment only
        hal::spi::Segment *segment = &copy[i];
        if (segment->make == nullptr) {
            continue;
        }

        // its bytes, each piece made into its piece buffer, then gathered
        uint8_t *bytes = (uint8_t *)memory::allocate(segment->size > 0 ? segment->size : 1);
        size_t offset = 0;

        while (offset < segment->size) {
            size_t left = segment->size - offset;
            size_t piece = left < segment->pieceSize ? left : segment->pieceSize;
            segment->make(segment->makeOpaque, segment->piece, offset, piece);
            memcpy(bytes + offset, segment->piece, piece);
            offset += piece;
        }

        // now a plain write segment, at the same DC level
        segment->write = bytes;
        segment->make = nullptr;
    }

    return copy;
}

/**
 * Frees a copy of makeSegments() ('count' segments): the bytes of the
 * segments that were made, then the copy. 'original' tells which ones.
 */
void freeMade(hal::spi::Segment *copy, const hal::spi::Segment *original, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (original[i].make != nullptr) {
            memory::release((void *)copy[i].write);
        }
    }

    memory::release(copy);
}

/**
 * How long a transaction lasts on the wire, in microseconds: its bytes
 * (written, read or exchanged: one byte per 8 clock ticks) at 'clockHz',
 * plus its delay segments.
 */
int64_t wireMicros(const hal::spi::Segment *segments, size_t count, uint32_t clockHz) {
    // its bytes and its delays
    uint64_t bytes = 0;
    int64_t delays = 0;

    for (size_t i = 0; i < count; i++) {
        bytes += segments[i].size;
        delays += segments[i].delayUs;
    }

    // the bytes at the clock
    int64_t onWire = clockHz > 0 ? (int64_t)(bytes * 8 * 1000000 / clockHz) : 0;
    return onWire + delays;
}

/**
 * Waits for the wire time of a transaction of bus 'bus' (index in
 * 'ports'): what it owes, added to what the bus still owed, in whole
 * milliseconds; the rest stays owed. Not under the lock.
 */
void waitWire(int bus, int64_t micros) {
    // what the bus owes now, the whole milliseconds taken out
    lock.lock();
    int64_t owed = buses[bus].owedMicros + micros;
    int64_t waited = owed / 1000;
    buses[bus].owedMicros = owed - waited * 1000;
    lock.unlock();

    // waited for
    if (waited > 0) {
        platform::sleepMs(waited);
    }
}

/**
 * Runs a transaction (run(), and the "twin spi" thread for start()):
 * 1. the device, which must exist;
 * 2. its made segments made, piece by piece, into plain write segments
 *    (a piece too large: failed, as on the ESP32);
 * 3. the bytes read at 0xFF: what MISO gives with nothing driving it;
 * 4. the component on its chip select, if any, answers;
 * 5. kept in the trace;
 * 6. what is left of its time on the wire waited for.
 */
hal::spi::Status transact(int device, const hal::spi::Segment *given, size_t count) {
    // when it starts: its time on the wire counts from there
    int64_t startedMs = platform::monotonicMs();

    // the device, and the largest transfer of its bus
    lock.lock();
    bool known = device >= 0 && device < DEVICES && devices[device].used;
    Device target = known ? devices[device] : Device{};
    size_t maxTransfer = known ? buses[target.bus].maxTransfer : 0;
    lock.unlock();
    if (!known) {
        return hal::spi::Status::failed;
    }

    // the made segments made now (a copy), the others as given
    const hal::spi::Segment *segments = given;
    hal::spi::Segment *made = nullptr;
    if (hasMadeSegment(given, count)) {
        made = makeSegments(given, count, maxTransfer);
        if (made == nullptr) {
            return hal::spi::Status::failed;
        }

        segments = made;
    }

    // the bytes read at 0xFF
    for (size_t i = 0; i < count; i++) {
        if (segments[i].read != nullptr) {
            memset(segments[i].read, 0xFF, segments[i].size);
        }
    }

    // the component that hears it, if any, answers
    int port = ports[target.bus];
    int listener = twin::spiListener(port, target.config.cs);
    twin::SpiOutcome outcome = twin::SpiOutcome::nobody;
    if (listener >= 0) {
        twin::SpiTransfer transfer = { port, listener, target.actualHz, target.config.mode, target.config.lsbFirst };
        outcome = twin::spiTransaction(&transfer, segments, count, ANSWER_TIMEOUT_MS);
    }

    // kept in the trace, its time on the wire counted; the bytes made freed
    record(&target, segments, count, outcome);
    int64_t micros = wireMicros(segments, count, target.actualHz);
    if (made != nullptr) {
        freeMade(made, given, count);
    }

    // what is left of its time on the wire
    int64_t spentMicros = (platform::monotonicMs() - startedMs) * 1000;
    waitWire(target.bus, micros > spentMicros ? micros - spentMicros : 0);
    return hal::spi::Status::ok;
}

/**
 * The "twin spi" thread: waits for a job of start(), runs it, calls its
 * 'done' (outside the lock).
 */
void runJobs(void *) {
    lock.lock();

    for (;;) {
        // the first job waiting
        int next = -1;

        for (int i = 0; i < twin::SPI_MAX_PORTS && next < 0; i++) {
            if (jobs[i].pending) {
                next = i;
            }
        }

        // none: wait for one
        if (next < 0) {
            lock.wait(-1);
            continue;
        }

        // taken, run, then its done called
        Job job = jobs[next];
        jobs[next].pending = false;
        lock.unlock();
        hal::spi::Status status = transact(job.device, job.segments, job.count);
        job.done(job.opaque, status);
        lock.lock();
    }
}

} // namespace

namespace twin {

/**
 * Reads "spi" of the description: { ports: [numbers], reserved: [numbers] }.
 */
bool spiRead(JSContext *ctx, JSValueConst description, const char *path) {
    // no "spi": the board has no SPI controller
    JSValue spi = JS_GetPropertyStr(ctx, description, "spi");
    if (JS_IsUndefined(spi)) {
        return true;
    }

    // the two lists
    portCount = readIntegers(ctx, spi, "ports", 0, 255, ports, SPI_MAX_PORTS);
    reservedCount = readIntegers(ctx, spi, "reserved", 0, 255, reserved, SPI_MAX_PORTS);
    JS_FreeValue(ctx, spi);
    bool valid = portCount >= 0 && reservedCount >= 0;
    if (!valid) {
        fprintf(stderr, "qjsx agent: %s: \"spi\" is { ports: [controller numbers], reserved: [controller numbers] }, %d "
                "each at most\n", path, SPI_MAX_PORTS);
        return false;
    }

    return true;
}

/**
 * The state of the buses, with their devices.
 */
int spiBusStates(SpiBusState *states) {
    lock.lock();

    for (int i = 0; i < portCount; i++) {
        // the bus and its pins
        SpiBusState *state = &states[i];
        *state = SpiBusState{};
        state->port = ports[i];
        state->open = buses[i].open;
        state->sclk = buses[i].sclk;
        state->mosi = buses[i].mosi;
        state->miso = buses[i].miso;

        // its devices
        for (const Device &device : devices) {
            bool onBus = device.used && device.bus == i;
            bool hasRoom = state->deviceCount < SPI_MAX_DEVICES;
            if (onBus && hasRoom) {
                const hal::spi::DeviceConfig *config = &device.config;
                SpiDeviceState shown = { config->cs, config->dc, device.actualHz, (uint8_t)config->mode,
                                         config->lsbFirst };
                state->devices[state->deviceCount++] = shown;
            }
        }
    }

    int count = portCount;
    lock.unlock();
    return count;
}

/**
 * The records after 'after', oldest first.
 */
uint32_t spiTrace(uint32_t after, SpiTraceVisitor *visit, void *context) {
    lock.lock();
    uint32_t newest = trace.visit(after, visit, context);
    lock.unlock();
    return newest;
}

} // namespace twin

namespace hal {
namespace spi {

/**
 * A controller of "ports" of the description.
 */
bool portExists(int port) {
    return indexIn(ports, portCount, port) >= 0;
}

/**
 * Takes a controller on pins of the board:
 * 1. a controller of "ports" ("reserved": busy; others: none);
 * 2. a clock pin, at least one data pin, all distinct pins of the board;
 * 3. free; then the page sees its pins.
 */
Status openBus(int port, int sclk, int mosi, int miso, size_t maxTransfer) {
    // a controller the program may open
    int index = indexIn(ports, portCount, port);
    if (index < 0) {
        bool isReserved = indexIn(reserved, reservedCount, port) >= 0;
        return isReserved ? Status::busy : Status::noSuchPort;
    }

    // the pins: a clock, a data line at least, each a pin of the board
    bool mosiValid = mosi == -1 || isBoardPin(mosi);
    bool misoValid = miso == -1 || isBoardPin(miso);
    bool hasData = mosi >= 0 || miso >= 0;
    bool distinct = sclk != mosi && sclk != miso && (mosi != miso || mosi == -1);
    bool pinsValid = isBoardPin(sclk) && mosiValid && misoValid && hasData && distinct;
    if (!pinsValid) {
        return Status::badPin;
    }

    // taken if free
    lock.lock();
    bool free = !buses[index].open;
    if (free) {
        buses[index] = Bus{ true, sclk, mosi, miso, maxTransfer, 0 };
    }

    lock.unlock();
    if (!free) {
        return Status::busy;
    }

    // the page sees its pins
    showPins();
    return Status::ok;
}

/**
 * Frees a controller and its pins.
 */
void closeBus(int port) {
    // a controller of the board
    int index = indexIn(ports, portCount, port);
    if (index < 0) {
        return;
    }

    // freed, its pins with it
    lock.lock();
    buses[index].open = false;
    lock.unlock();
    showPins();
}

/**
 * A device on an open bus:
 * 1. the bus open, the settings within reach;
 * 2. its chip select and DC pins: pins of the board, not those of the bus;
 * 3. a chip select of the controller left, a free entry;
 * 4. its clock rounded as the hardware does; the page sees its pins.
 */
Status addDevice(int port, const DeviceConfig *config, int *device, uint32_t *actualHz) {
    // the bus, the settings
    int index = indexIn(ports, portCount, port);
    bool settingsValid = config->frequencyHz > 0 && config->mode >= 0 && config->mode <= 3;
    if (index < 0 || !settingsValid) {
        return Status::failed;
    }

    lock.lock();
    const Bus *bus = &buses[index];
    if (!bus->open) {
        lock.unlock();
        return Status::failed;
    }

    // its pins: none (-1), or pins of the board other than those of the bus
    bool csValid = config->cs == -1 || (isBoardPin(config->cs) && !isBusPin(bus, config->cs));
    bool dcValid = config->dc == -1 || (isBoardPin(config->dc) && !isBusPin(bus, config->dc));
    bool distinct = config->cs != config->dc || config->cs == -1;
    if (!csValid || !dcValid || !distinct) {
        lock.unlock();
        return Status::badPin;
    }

    // the devices of that bus, and the first free entry
    int onBus = 0;
    int found = -1;

    for (int i = 0; i < DEVICES; i++) {
        if (devices[i].used && devices[i].bus == index) {
            onBus++;
        } else if (!devices[i].used && found < 0) {
            found = i;
        }
    }

    // a chip select of the controller left, and an entry
    bool hasRoom = onBus < twin::SPI_MAX_DEVICES && found >= 0;
    if (!hasRoom) {
        lock.unlock();
        return Status::busy;
    }

    // taken, at the clock the hardware makes
    uint32_t clock = actualClock(config->frequencyHz);
    devices[found] = Device{ true, index, *config, clock };
    lock.unlock();

    *device = found;
    *actualHz = clock;
    showPins();
    return Status::ok;
}

/**
 * Frees a device; the page does not see its pins any more.
 */
void removeDevice(int device) {
    // an entry of the table
    bool deviceValid = device >= 0 && device < DEVICES;
    if (!deviceValid) {
        return;
    }

    // freed
    lock.lock();
    devices[device].used = false;
    lock.unlock();
    showPins();
}

/**
 * A transaction, answered by the component on its chip select.
 */
Status run(int device, const Segment *segments, size_t count) {
    return transact(device, segments, count);
}

/**
 * Hands a transaction to the "twin spi" thread (started at the first one);
 * 'done' is called at once when it cannot start.
 */
void start(int device, const Segment *segments, size_t count, DoneCallback done, void *opaque) {
    // the device and its bus: the bus's job slot
    lock.lock();
    bool known = device >= 0 && device < DEVICES && devices[device].used;
    int bus = known ? devices[device].bus : -1;
    bool slotFree = known && !jobs[bus].pending;

    // the thread, once
    if (slotFree && !threadStarted) {
        threadStarted = platform::startThread(&runJobs, nullptr, platform::defaultThreadOptions("twin spi"));
    }

    // the job, and the thread woken
    bool queued = slotFree && threadStarted;
    if (queued) {
        jobs[bus] = Job{ true, device, segments, count, done, opaque };
        lock.notify();
    }

    lock.unlock();

    // could not start: said at once
    if (!queued) {
        done(opaque, Status::failed);
    }
}

/**
 * Nothing to free: the job was taken off by the thread before 'done'.
 */
void finish(int) {
}

} // namespace spi
} // namespace hal
