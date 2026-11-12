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
    int  sda;                            // its pins (open)
    int  scl;
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
twin::I2cRecord trace[twin::I2C_TRACE_RECORDS];
uint32_t traceSequence = 0;              // number of the newest record; 0: none
int64_t traceStartMs = -1;               // when the twin started (the first open)

/**
 * Keeps a transfer in the trace (the oldest record goes when it is full).
 */
void record(int bus, int address, uint32_t frequencyHz, const uint8_t *written, size_t writeSize, const uint8_t *read,
            size_t readSize, hal::i2c::Status status) {
    lock.lock();

    // the slot of the new record, overwriting the oldest
    traceSequence++;
    twin::I2cRecord *entry = &trace[traceSequence % twin::I2C_TRACE_RECORDS];
    memset(entry, 0, sizeof(*entry));
    entry->sequence = traceSequence;
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
            if (onBus && !known && state->frequencyCount < I2C_MAX_FREQUENCIES) {
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

    // the oldest record still in the ring
    uint32_t oldest = traceSequence > (uint32_t)I2C_TRACE_RECORDS ? traceSequence - I2C_TRACE_RECORDS + 1 : 1;
    uint32_t first = after + 1 > oldest ? after + 1 : oldest;
    for (uint32_t sequence = first; sequence <= traceSequence; sequence++) {
        visit(context, &trace[sequence % I2C_TRACE_RECORDS]);
    }

    uint32_t newest = traceSequence;
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
    bool pinsValid = sda >= 0 && sda < twin::pinCount() && scl >= 0 && scl < twin::pinCount() && sda != scl;
    if (bus < 0 || bus >= twin::I2C_BUSES || !pinsValid) {
        return false;
    }

    lock.lock();
    bool free = !buses[bus].open;
    if (free) {
        buses[bus] = Bus{ true, sda, scl };
    }
    if (traceStartMs < 0) {
        traceStartMs = platform::monotonicMs();
    }
    lock.unlock();

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
    if (bus < 0 || bus >= twin::I2C_BUSES) {
        return;
    }

    lock.lock();
    Bus closed = buses[bus];
    buses[bus].open = false;
    lock.unlock();

    if (closed.open) {
        twin::setI2c(closed.sda, false);
        twin::setI2c(closed.scl, false);
    }
}

/**
 * A device on an open bus: the first free entry.
 */
int addDevice(int bus, int address, uint32_t frequencyHz) {
    if (bus < 0 || bus >= twin::I2C_BUSES) {
        return -1;
    }

    lock.lock();
    int found = -1;
    for (int i = 0; i < DEVICES && found < 0 && buses[bus].open; i++) {
        if (!devices[i].used) {
            found = i;
        }
    }
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
    if (device < 0 || device >= DEVICES) {
        return;
    }

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
    record(target.bus, target.address, target.frequencyHz, writeData, writeSize, readData, readSize, status);
    return status;
}

/**
 * Whether a component answers at 'address': a transfer of nothing, kept in
 * the trace.
 */
Status probe(int bus, int address, int timeoutMs) {
    lock.lock();
    bool open = bus >= 0 && bus < twin::I2C_BUSES && buses[bus].open;
    Bus pins = open ? buses[bus] : Bus{};
    lock.unlock();
    if (!open) {
        return Status::failed;
    }

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
