/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The bridge between the simulated hardware and the models of the parts
// (see twin.h): the events posted to the twin's runtime, the places the
// components answer at (an I2C address, an SPI chip select), and the
// transfers waiting for an answer (I2C transfers, SPI transactions).
//
// Threads: bridgePost, i2cTransfer, spiListener and spiTransaction on any
// thread (the program's for a transfer); bridgeAttach, i2cAttach, spiAttach,
// i2cAnswer and spiAnswer on the twin's thread.
#include "twin.h"
#include "hal_gpio.h"
#include "memory.h"
#include "platform.h"

#include <string.h>

namespace twin {

namespace {

const int MAX_ATTACHED = 64;          // places answered by components, every bus together
const int MAX_PENDING = 4;            // transfers waiting for an answer at the same time
const size_t TRANSFER_HEADER = 8;     // [sda, scl, address, 0, readSize u32 LE] before the bytes written
const uint8_t SPI_IDLE_BYTE = 0xFF;   // what MISO gives when nothing drives it (its pull-up)

// The bridge of the twin's runtime (bridgeLock).
rt::Mailbox *bridgeMailbox = nullptr;     // retained; nullptr: no bridge
rt::SourceId bridgeSource = { -1, 0 };    // the bridge object's source in that loop
platform::Mutex bridgeLock;               // bridgeMailbox, bridgeSource

// A place a component answers at (attachLock): an I2C address on the bus of
// two pins, or an SPI chip select on a controller.
struct Attached {
    bool        used;                     // the entry is taken
    BridgeEvent bus;                      // BridgeEvent::i2cTransfer or BridgeEvent::spiTransaction
    int         first;                    // I2C: the data pin; SPI: the port
    int         second;                   // I2C: the clock pin; SPI: the chip select pin
    int         third;                    // I2C: the 7-bit address; SPI: 0
};

Attached attached[MAX_ATTACHED];          // attachLock
platform::Mutex attachLock;               // attached

// A transfer waiting for the answer of a component. Each has its own
// Signal: its lock guards the fields, its condition wakes the waiting
// program's thread.
struct Pending {
    platform::Signal signal;              // lock of the fields below, and the wake up
    bool     used;                        // a transfer waits on it
    uint32_t request;                     // the number of that transfer (0: none)
    bool     answered;                    // the answer came
    int      status;                      // hal::i2c::Status as a number (answered; I2C only)
    uint8_t *read;                        // the bytes read (answered), owned; nullptr: none
    size_t   readSize;                    // bytes asked
    uint8_t  fill;                        // what the bytes the component does not give read as
};

Pending pending[MAX_PENDING];             // each under its own signal
platform::Mutex requestLock;              // nextRequest, and taking a free 'pending'
uint32_t nextRequest = 1;                 // number of the next transfer (requestLock)

// How an exchange with the components ended (exchange()).
enum class Ending {
    answered,                             // the component answered: its status, its bytes
    unposted,                             // no bridge: nobody heard it
    noSlot,                               // too many transfers waiting already
    timedOut,                             // no answer within the time limit
};

// The report of the twin's runtime: its parts built or not (readySignal).
platform::Signal readySignal;             // lock of the two below, and the wake up
bool readyDone = false;                   // readyReport() came
char *readyError = nullptr;               // why the parts could not be built, owned; nullptr: built

/**
 * The entry of a place, -1 if no component answers there (attachLock held).
 */
int findAttached(BridgeEvent bus, int first, int second, int third) {
    for (int i = 0; i < MAX_ATTACHED; i++) {
        const Attached *entry = &attached[i];
        bool samePlace = entry->bus == bus && entry->first == first && entry->second == second && entry->third == third;
        if (entry->used && samePlace) {
            return i;
        }
    }

    return -1;
}

/**
 * Whether a component answers at a place.
 */
bool isAttached(BridgeEvent bus, int first, int second, int third) {
    attachLock.lock();
    bool found = findAttached(bus, first, second, third) >= 0;
    attachLock.unlock();
    return found;
}

/**
 * Adds or removes a place a component answers at.
 */
void setAttached(BridgeEvent bus, int first, int second, int third, bool attach) {
    attachLock.lock();

    // the entry of that place, or a free one to take
    int found = findAttached(bus, first, second, third);
    int free = -1;

    for (int i = 0; i < MAX_ATTACHED && free < 0; i++) {
        if (!attached[i].used) {
            free = i;
        }
    }

    // attach: take a free entry if not there yet; detach: let it go
    bool canTake = found < 0 && free >= 0;
    if (attach && canTake) {
        attached[free] = Attached{ true, bus, first, second, third };
    } else if (!attach && found >= 0) {
        attached[found].used = false;
    }

    attachLock.unlock();
}

/**
 * Writes 'value' at 'bytes', little endian, on 4 bytes.
 */
void putU32(uint8_t *bytes, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        bytes[i] = (uint8_t)(value >> (8 * i));
    }
}

/**
 * Takes a free waiting slot for a new transfer: its index and number in
 * *request; -1 when every slot is taken.
 */
int takePending(uint32_t *request) {
    requestLock.lock();
    int found = -1;

    for (int i = 0; i < MAX_PENDING && found < 0; i++) {
        // the first free slot, made ready for the next request number
        Pending *slot = &pending[i];
        slot->signal.lock();
        if (!slot->used) {
            found = i;
            *request = nextRequest++;
            slot->used = true;
            slot->request = *request;
            slot->answered = false;
            slot->read = nullptr;
        }

        slot->signal.unlock();
    }

    requestLock.unlock();
    return found;
}

/**
 * Lets a waiting slot go (its signal's lock held): frees what it holds.
 */
void freePending(Pending *slot) {
    memory::release(slot->read);
    slot->read = nullptr;
    slot->used = false;
    slot->request = 0;
}

/**
 * A request to the components and its answer:
 * 1. a waiting slot, told what is to read;
 * 2. the request posted to the bridge ('message': its data);
 * 3. wait for the answer, at most timeoutMs;
 * 4. copy what was read into 'read' ('readSize' bytes; untouched without an
 *    answer) and its status into *status, let the slot go.
 */
Ending exchange(BridgeEvent kind, const uint8_t *message, size_t size, uint8_t *read, size_t readSize, uint8_t fill,
                int timeoutMs, int *status) {
    // a slot
    uint32_t request = 0;
    int index = takePending(&request);
    if (index < 0) {
        return Ending::noSlot;
    }

    // what to read set before the post (the answer reads it), then posted
    Pending *slot = &pending[index];
    slot->signal.lock();
    slot->readSize = readSize;
    slot->fill = fill;
    slot->signal.unlock();
    bool posted = bridgePost(kind, request, message, size);

    // wait for the answer (or the time limit); the answer may already be there
    int64_t waitMs = timeoutMs > 0 ? timeoutMs : 0;
    int64_t limit = platform::monotonicMs() + waitMs;
    slot->signal.lock();

    while (posted && !slot->answered) {
        int64_t left = limit - platform::monotonicMs();
        if (left <= 0) {
            break;
        }

        slot->signal.wait(left);
    }

    // how it ended, what came, then the slot freed (a late answer finds it gone)
    Ending ending = Ending::timedOut;
    if (!posted) {
        ending = Ending::unposted;
    } else if (slot->answered) {
        ending = Ending::answered;
        *status = slot->status;
        if (slot->read != nullptr) {
            memcpy(read, slot->read, readSize);
        }
    }

    freePending(slot);
    slot->signal.unlock();
    return ending;
}

/**
 * The answer of a component, copied into the slot of that request if it
 * still waits ('carriesBytes': the answer has bytes read, the rest of what
 * was asked reads as the slot's fill byte), then the waiting thread woken.
 */
void answer(uint32_t request, int status, bool carriesBytes, const uint8_t *bytes, size_t size) {
    for (int i = 0; i < MAX_PENDING; i++) {
        Pending *slot = &pending[i];
        slot->signal.lock();
        bool waiting = slot->used && slot->request == request && !slot->answered;
        if (waiting) {
            // the bytes read: what the component gave, the fill byte after
            slot->status = status;
            bool hasBytes = carriesBytes && slot->readSize > 0;
            if (hasBytes) {
                size_t given = size < slot->readSize ? size : slot->readSize;
                slot->read = (uint8_t *)memory::allocate(slot->readSize);
                memset(slot->read, slot->fill, slot->readSize);
                memcpy(slot->read, bytes, given);
            }

            // answered: the waiting thread woken
            slot->answered = true;
            slot->signal.notify();
        }

        slot->signal.unlock();
    }
}

} // namespace

/**
 * The bridge: keeps the mailbox (retained) and the source.
 */
void bridgeAttach(rt::Mailbox *mailbox, rt::SourceId source) {
    // the new mailbox retained and kept
    mailbox->retain();
    bridgeLock.lock();
    rt::Mailbox *previous = bridgeMailbox;
    bridgeMailbox = mailbox;
    bridgeSource = source;
    bridgeLock.unlock();

    // the previous one released, outside the lock
    if (previous != nullptr) {
        previous->release();
    }
}

/**
 * No bridge any more.
 */
void bridgeDetach() {
    // no mailbox any more: events dropped from now on
    bridgeLock.lock();
    rt::Mailbox *previous = bridgeMailbox;
    bridgeMailbox = nullptr;
    bridgeLock.unlock();

    // the previous one released, outside the lock
    if (previous != nullptr) {
        previous->release();
    }
}

/**
 * Posts a copy of 'data' to the bridge.
 */
bool bridgePost(BridgeEvent kind, int64_t value, const void *data, size_t size) {
    bridgeLock.lock();
    bool hasBridge = bridgeMailbox != nullptr;
    if (hasBridge) {
        // the loop frees the copy after dispatch
        uint8_t *copy = nullptr;
        if (size > 0) {
            copy = (uint8_t *)memory::allocate(size);
            memcpy(copy, data, size);
        }

        bridgeMailbox->post(bridgeSource, (int)kind, copy, size, value);
    }

    bridgeLock.unlock();
    return hasBridge;
}

/**
 * Adds or removes an address a component answers at.
 */
void i2cAttach(int sda, int scl, int address, bool attach) {
    setAttached(BridgeEvent::i2cTransfer, sda, scl, address, attach);
}

/**
 * The program's transfer:
 * 1. no component at that address: a NACK at once;
 * 2. the request, [sda, scl, address, 0, readSize u32 LE] then the bytes
 *    written, exchanged with the components;
 * 3. its ending as a status.
 */
hal::i2c::Status i2cTransfer(int sda, int scl, int address, const uint8_t *written, size_t writeSize,
                             uint8_t *read, size_t readSize, int timeoutMs) {
    // nobody at that address
    if (!isAttached(BridgeEvent::i2cTransfer, sda, scl, address)) {
        return hal::i2c::Status::noAck;
    }

    // the request: the header then the bytes written
    size_t size = TRANSFER_HEADER + writeSize;
    uint8_t *message = (uint8_t *)memory::allocate(size);
    message[0] = (uint8_t)sda;
    message[1] = (uint8_t)scl;
    message[2] = (uint8_t)address;
    message[3] = 0;
    putU32(message + 4, (uint32_t)readSize);
    memcpy(message + TRANSFER_HEADER, written, writeSize);

    // exchanged (the bytes the component does not give read as 0)
    int status = (int)hal::i2c::Status::failed;
    Ending ending = exchange(BridgeEvent::i2cTransfer, message, size, read, readSize, 0, timeoutMs, &status);
    memory::release(message);

    // its ending as a status
    switch (ending) {
        case Ending::answered: {
            return (hal::i2c::Status)status;
        }
        case Ending::unposted: {
            return hal::i2c::Status::noAck;
        }
        case Ending::noSlot: {
            return hal::i2c::Status::failed;
        }
        case Ending::timedOut: {
            return hal::i2c::Status::timeout;
        }
    }

    return hal::i2c::Status::failed;
}

/**
 * The answer of a component to an I2C transfer.
 */
void i2cAnswer(uint32_t request, int status, const uint8_t *bytes, size_t size) {
    // the bytes count only for a transfer that went through
    bool carriesBytes = status == (int)hal::i2c::Status::ok;
    answer(request, status, carriesBytes, bytes, size);
}

/**
 * Adds or removes a chip select a component sits on.
 */
void spiAttach(int port, int cs, bool attach) {
    setAttached(BridgeEvent::spiTransaction, port, cs, 0, attach);
}

/**
 * The component that hears a transaction: the one on the device's chip
 * select, or, for a device without one, the first whose chip select pin the
 * program holds low.
 */
int spiListener(int port, int cs) {
    // a chip select driven by the hardware: the component on it, if any
    if (cs >= 0) {
        return isAttached(BridgeEvent::spiTransaction, port, cs, 0) ? cs : -1;
    }

    // driven by the program: the chip selects of the components of that
    // controller, copied (their levels are read outside the lock)
    int candidates[MAX_ATTACHED];
    int count = 0;
    attachLock.lock();

    for (const Attached &entry : attached) {
        bool onPort = entry.used && entry.bus == BridgeEvent::spiTransaction && entry.first == port;
        if (onPort) {
            candidates[count++] = entry.second;
        }
    }

    attachLock.unlock();

    // the first one selected: its line low
    for (int i = 0; i < count; i++) {
        if (hal::gpio::read(candidates[i]) == 0) {
            return candidates[i];
        }
    }

    return -1;
}

/**
 * The program's transaction:
 * 1. the bytes written and read by its segments;
 * 2. the request: the header, a descriptor per segment, the bytes written;
 * 3. exchanged with the components, the bytes read at 0xFF first;
 * 4. what was read spread into the segments.
 */
SpiOutcome spiTransaction(const SpiTransfer *transfer, const hal::spi::Segment *segments, size_t count, int timeoutMs) {
    // the bytes written (write, transfer) and read (read, transfer)
    size_t writeTotal = 0;
    size_t readTotal = 0;

    for (size_t i = 0; i < count; i++) {
        const hal::spi::Segment *segment = &segments[i];
        writeTotal += segment->write != nullptr ? segment->size : 0;
        readTotal += segment->read != nullptr ? segment->size : 0;
    }

    // the header
    size_t size = SPI_MESSAGE_HEADER + count * SPI_MESSAGE_SEGMENT + writeTotal;
    uint8_t *message = (uint8_t *)memory::allocate(size);
    message[0] = (uint8_t)transfer->port;
    message[1] = (uint8_t)transfer->cs;
    message[2] = (uint8_t)transfer->mode;
    message[3] = transfer->lsbFirst ? 1 : 0;
    putU32(message + 4, transfer->frequencyHz);
    putU32(message + 8, (uint32_t)count);
    putU32(message + 12, (uint32_t)readTotal);

    // a descriptor per segment, then the bytes written, one after the other
    uint8_t *descriptor = message + SPI_MESSAGE_HEADER;
    uint8_t *written = descriptor + count * SPI_MESSAGE_SEGMENT;

    for (size_t i = 0; i < count; i++) {
        const hal::spi::Segment *segment = &segments[i];
        uint8_t *entry = descriptor + i * SPI_MESSAGE_SEGMENT;
        entry[0] = (uint8_t)spiKindOf(segment);
        entry[1] = (uint8_t)(segment->dc != 0 ? 1 : 0);
        entry[2] = 0;
        entry[3] = 0;
        putU32(entry + 4, (uint32_t)segment->size);
        putU32(entry + 8, segment->delayUs);
        if (segment->write != nullptr) {
            memcpy(written, segment->write, segment->size);
            written += segment->size;
        }
    }

    // exchanged; what the component does not give reads 0xFF (at least one
    // byte allocated: a transaction may read nothing)
    uint8_t *read = (uint8_t *)memory::allocate(readTotal > 0 ? readTotal : 1);
    memset(read, SPI_IDLE_BYTE, readTotal);
    int status = 0;
    Ending ending = exchange(BridgeEvent::spiTransaction, message, size, read, readTotal, SPI_IDLE_BYTE, timeoutMs,
                             &status);
    memory::release(message);

    // spread into the read and transfer segments, in their order
    size_t offset = 0;

    for (size_t i = 0; i < count; i++) {
        const hal::spi::Segment *segment = &segments[i];
        if (segment->read != nullptr) {
            memcpy(segment->read, read + offset, segment->size);
            offset += segment->size;
        }
    }

    memory::release(read);

    // how it went: no bridge is nobody; no slot left counts as no answer
    if (ending == Ending::answered) {
        return SpiOutcome::answered;
    }

    return ending == Ending::unposted ? SpiOutcome::nobody : SpiOutcome::timeout;
}

/**
 * The answer of a component to an SPI transaction: always bytes.
 */
void spiAnswer(uint32_t request, const uint8_t *bytes, size_t size) {
    answer(request, 0, true, bytes, size);
}

/**
 * The first report of the twin's runtime, kept, then the agent woken.
 */
void readyReport(const char *error) {
    readySignal.lock();
    if (!readyDone) {
        readyDone = true;
        readyError = nullptr;
        if (error != nullptr) {
            readyError = memory::copyText(error, strlen(error));
        }

        readySignal.notify();
    }

    readySignal.unlock();
}

/**
 * Waits for the report; gives its error away.
 */
bool readyWait(int64_t timeoutMs, char **error) {
    // wait for the report, at most timeoutMs
    int64_t limit = platform::monotonicMs() + timeoutMs;
    readySignal.lock();

    while (!readyDone) {
        int64_t left = limit - platform::monotonicMs();
        if (left <= 0) {
            break;
        }

        readySignal.wait(left);
    }

    // the error given to the caller
    bool done = readyDone;
    *error = readyError;
    readyError = nullptr;
    readySignal.unlock();
    return done;
}

} // namespace twin
