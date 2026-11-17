/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// hal::i2c of the digital twin (see twin.h): the buses open on the board's
// pins; a transfer goes to the component wired at that address on the bus
// of those pins (twin::i2cTransfer: the model of a part answers it), a NACK
// when there is none (ENXIO for the program). A probe is a transfer that
// writes and reads nothing. Every transfer is kept in a trace for the page
// (the last I2C_TRACE_RECORDS).
//
// Threads: any thread, as the hal requires; lock protects the tables and
// the trace.
#include "hal_i2c.h"
#include "twin.h"
#include "platform.h"

#include <string.h>

namespace {

const int DEVICES = 32;                  // devices of every bus together

// A bus (under lock).
struct Bus {
    bool open;                           // openBus() took it
    int  sda;                            // its data pin (open)
    int  scl;                            // its clock pin (open)
};

// A device added on a bus (under lock).
struct Device {
    bool     used;                       // addDevice() gave it
    int      bus;                        // its bus
    int      address;                    // its 7-bit address
    uint32_t frequencyHz;                // its clock
};

Bus buses[twin::I2C_BUSES];              // lock
Device devices[DEVICES];                 // lock
platform::Mutex lock;                    // buses, devices, the trace

// The trace: a ring of the last records (lock).
twin::TraceRing<twin::I2cRecord, twin::I2C_TRACE_RECORDS> trace;
int64_t traceStartMs = -1;               // when the twin started (the first open)

/**
 * Keeps a transfer in the trace (the oldest record goes when it is full).
 */
void record(int bus, int address, uint32_t frequencyHz, const uint8_t *written, size_t writeSize, const uint8_t *read,
            size_t readSize, hal::i2c::Status status) {
    lock.lock();

    // the slot of the new record, overwriting the oldest
    twin::I2cRecord *entry = trace.add();
    entry->timeMs = platform::monotonicMs() - traceStartMs;
    entry->bus = bus;
    entry->address = address;
    entry->status = (int)status;
    entry->frequencyHz = frequencyHz;

    // the first bytes written, and read when it went through
    entry->writeSize = writeSize;
    entry->readSize = readSize;
    size_t writeKept = writeSize < twin::I2C_TRACE_BYTES ? writeSize : twin::I2C_TRACE_BYTES;
    size_t readKept = readSize < twin::I2C_TRACE_BYTES ? readSize : twin::I2C_TRACE_BYTES;
    if (writeKept > 0) {
        memcpy(entry->written, written, writeKept);
    }

    if (status == hal::i2c::Status::ok && readKept > 0) {
        memcpy(entry->read, read, readKept);
    }

    lock.unlock();
}

} // namespace

namespace twin {

/**
 * The state of the buses, with the frequencies of their devices.
 */
void i2cBusStates(I2cBusState *states) {
    lock.lock();

    for (int i = 0; i < I2C_BUSES; i++) {
        // the bus and its pins
        I2cBusState *state = &states[i];
        *state = I2cBusState{};
        state->open = buses[i].open;
        state->sda = buses[i].sda;
        state->scl = buses[i].scl;

        // the frequencies of its devices, each once
        for (const Device &device : devices) {
            bool onBus = device.used && device.bus == i;
            bool known = false;

            for (int k = 0; k < state->frequencyCount; k++) {
                known = known || state->frequencies[k] == device.frequencyHz;
            }

            bool hasRoom = state->frequencyCount < I2C_MAX_FREQUENCIES;
            if (onBus && !known && hasRoom) {
                state->frequencies[state->frequencyCount++] = device.frequencyHz;
            }
        }
    }

    lock.unlock();
}

/**
 * The records after 'after', oldest first.
 */
uint32_t i2cTrace(uint32_t after, I2cTraceVisitor *visit, void *context) {
    lock.lock();
    uint32_t newest = trace.visit(after, visit, context);
    lock.unlock();
    return newest;
}

} // namespace twin

namespace hal {
namespace i2c {

/**
 * Buses of the twin.
 */
int busCount() {
    return twin::I2C_BUSES;
}

/**
 * Takes a bus on two pins of the board; the page sees them as I2C pins.
 */
bool openBus(int bus, int sda, int scl, bool) {
    // a bus of the twin, two distinct pins of the board
    bool sdaValid = sda >= 0 && sda < twin::pinCount();
    bool sclValid = scl >= 0 && scl < twin::pinCount();
    bool pinsValid = sdaValid && sclValid && sda != scl;
    bool busValid = bus >= 0 && bus < twin::I2C_BUSES;
    if (!busValid || !pinsValid) {
        return false;
    }

    // taken if free
    lock.lock();
    bool free = !buses[bus].open;
    if (free) {
        buses[bus] = Bus{ true, sda, scl };
    }

    // the time 0 of the trace, at the first open
    if (traceStartMs < 0) {
        traceStartMs = platform::monotonicMs();
    }

    lock.unlock();

    // the page sees its pins as I2C pins
    if (free) {
        twin::setI2c(sda, true);
        twin::setI2c(scl, true);
    }

    return free;
}

/**
 * Frees a bus and its pins.
 */
void closeBus(int bus) {
    // a bus of the twin
    bool busValid = bus >= 0 && bus < twin::I2C_BUSES;
    if (!busValid) {
        return;
    }

    // freed
    lock.lock();
    Bus closed = buses[bus];
    buses[bus].open = false;
    lock.unlock();

    // its pins are not I2C pins any more
    if (closed.open) {
        twin::setI2c(closed.sda, false);
        twin::setI2c(closed.scl, false);
    }
}

/**
 * A device on an open bus: the first free entry.
 */
int addDevice(int bus, int address, uint32_t frequencyHz) {
    // a bus of the twin
    bool busValid = bus >= 0 && bus < twin::I2C_BUSES;
    if (!busValid) {
        return -1;
    }

    // the first free entry, when the bus is open
    lock.lock();
    int found = -1;

    for (int i = 0; i < DEVICES && found < 0 && buses[bus].open; i++) {
        if (!devices[i].used) {
            found = i;
        }
    }

    // taken
    if (found >= 0) {
        devices[found] = Device{ true, bus, address, frequencyHz };
    }

    lock.unlock();
    return found;
}

/**
 * Frees a device.
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
}

/**
 * A transfer with a device: answered by the component at its address on the
 * bus of its pins, then kept in the trace.
 */
Status transfer(int device, const uint8_t *writeData, size_t writeSize, uint8_t *readData, size_t readSize,
                int timeoutMs) {
    // the device, its bus and its pins
    lock.lock();
    bool known = device >= 0 && device < DEVICES && devices[device].used;
    Device target = known ? devices[device] : Device{};
    Bus bus = known ? buses[target.bus] : Bus{};
    lock.unlock();
    if (!known || !bus.open) {
        return Status::failed;
    }

    // the component there answers it
    Status status = twin::i2cTransfer(bus.sda, bus.scl, target.address, writeData, writeSize, readData, readSize,
                                      timeoutMs);

    // kept in the trace
    record(target.bus, target.address, target.frequencyHz, writeData, writeSize, readData, readSize, status);
    return status;
}

/**
 * Whether a component answers at 'address': a transfer of nothing, kept in
 * the trace.
 */
Status probe(int bus, int address, int timeoutMs) {
    // an open bus of the twin, and its pins
    lock.lock();
    bool open = bus >= 0 && bus < twin::I2C_BUSES && buses[bus].open;
    Bus pins = open ? buses[bus] : Bus{};
    lock.unlock();
    if (!open) {
        return Status::failed;
    }

    // a transfer of nothing, kept in the trace (frequency 0: a probe)
    Status status = twin::i2cTransfer(pins.sda, pins.scl, address, nullptr, 0, nullptr, 0, timeoutMs);
    record(bus, address, 0, nullptr, 0, nullptr, 0, status);
    return status;
}

/**
 * Nothing to free: the components of the twin never hold a bus.
 */
void resetBus(int) {
}

} // namespace i2c
} // namespace hal
