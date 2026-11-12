/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The bridge between the simulated hardware and the models of the parts
// (see twin.h): the events posted to the twin's runtime, the I2C addresses
// the components answer at, and the I2C transfers waiting for an answer.
//
// Threads: bridgePost and i2cTransfer on any thread (the program's for a
// transfer); bridgeAttach, i2cAttach and i2cAnswer on the twin's thread.
#include "twin.h"
#include "memory.h"
#include "platform.h"

#include <string.h>

namespace twin {

namespace {

const int MAX_ATTACHED = 64;          // I2C addresses answered by components, every bus together
const int MAX_PENDING = 4;            // I2C transfers waiting for an answer at the same time
const size_t TRANSFER_HEADER = 8;     // [sda, scl, address, 0, readSize u32 LE] before the bytes written

// The bridge of the twin's runtime (bridgeLock).
rt::Mailbox *bridgeMailbox = nullptr;     // retained; nullptr: no bridge
rt::SourceId bridgeSource = { -1, 0 };    // the bridge object's source in that loop
platform::Mutex bridgeLock;               // bridgeMailbox, bridgeSource

// An I2C address a component answers at (attachLock).
struct Attached {
    bool used;                            // the entry is taken
    int  sda;                             // the bus, by its pins
    int  scl;
    int  address;                         // 7-bit address
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
    bool     answered;                    // i2cAnswer came
    int      status;                      // hal::i2c::Status as a number (answered)
    uint8_t *read;                        // the bytes read (answered), owned; nullptr: none
    size_t   readSize;                    // bytes asked
};

Pending pending[MAX_PENDING];             // each under its own signal
platform::Mutex requestLock;              // nextRequest, and taking a free 'pending'
uint32_t nextRequest = 1;                 // number of the next transfer (requestLock)

// The report of the twin's runtime: its parts built or not (readySignal).
platform::Signal readySignal;             // lock of the two below, and the wake up
bool readyDone = false;                   // readyReport() came
char *readyError = nullptr;               // why the parts could not be built, owned; nullptr: built

/**
 * Whether a component answers at 'address' on the bus of 'sda' / 'scl'.
 */
bool isAttached(int sda, int scl, int address) {
    attachLock.lock();
    bool found = false;
    for (int i = 0; i < MAX_ATTACHED && !found; i++) {
        const Attached *entry = &attached[i];
        found = entry->used && entry->sda == sda && entry->scl == scl && entry->address == address;
    }

    attachLock.unlock();
    return found;
}

/**
 * Takes a free waiting slot for a new transfer: its index and number in
 * *request; -1 when every slot is taken.
 */
int takePending(uint32_t *request) {
    requestLock.lock();
    int found = -1;
    for (int i = 0; i < MAX_PENDING && found < 0; i++) {
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

} // namespace

/**
 * The bridge: keeps the mailbox (retained) and the source.
 */
void bridgeAttach(rt::Mailbox *mailbox, rt::SourceId source) {
    mailbox->retain();
    bridgeLock.lock();
    rt::Mailbox *previous = bridgeMailbox;
    bridgeMailbox = mailbox;
    bridgeSource = source;
    bridgeLock.unlock();

    if (previous != nullptr) {
        previous->release();
    }
}

/**
 * No bridge any more.
 */
void bridgeDetach() {
    bridgeLock.lock();
    rt::Mailbox *previous = bridgeMailbox;
    bridgeMailbox = nullptr;
    bridgeLock.unlock();

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
    attachLock.lock();

    // the entry of that address, or a free one to take
    int found = -1;
    int free = -1;
    for (int i = 0; i < MAX_ATTACHED; i++) {
        const Attached *entry = &attached[i];
        bool same = entry->used && entry->sda == sda && entry->scl == scl && entry->address == address;
        if (same) {
            found = i;
        } else if (!entry->used && free < 0) {
            free = i;
        }
    }

    // attach: take a free entry if not there yet; detach: let it go
    if (attach && found < 0 && free >= 0) {
        attached[free] = Attached{ true, sda, scl, address };
    } else if (!attach && found >= 0) {
        attached[found].used = false;
    }

    attachLock.unlock();
}

/**
 * The program's transfer:
 * 1. no component at that address: a NACK at once;
 * 2. a waiting slot, the request posted to the bridge;
 * 3. wait for the answer, at most timeoutMs;
 * 4. copy what was read, let the slot go.
 */
hal::i2c::Status i2cTransfer(int sda, int scl, int address, const uint8_t *written, size_t writeSize,
                             uint8_t *read, size_t readSize, int timeoutMs) {
    // nobody at that address
    if (!isAttached(sda, scl, address)) {
        return hal::i2c::Status::noAck;
    }

    // a slot, and the request: the header then the bytes written
    uint32_t request = 0;
    int index = takePending(&request);
    if (index < 0) {
        return hal::i2c::Status::failed;
    }

    Pending *slot = &pending[index];
    size_t size = TRANSFER_HEADER + writeSize;
    uint8_t *message = (uint8_t *)memory::allocate(size);
    message[0] = (uint8_t)sda;
    message[1] = (uint8_t)scl;
    message[2] = (uint8_t)address;
    message[3] = 0;
    for (int i = 0; i < 4; i++) {
        message[4 + i] = (uint8_t)(readSize >> (8 * i));
    }
    memcpy(message + TRANSFER_HEADER, written, writeSize);

    slot->signal.lock();
    slot->readSize = readSize;
    slot->signal.unlock();
    bool posted = bridgePost(BridgeEvent::i2cTransfer, request, message, size);
    memory::release(message);

    // wait for the answer (or the time limit); the answer may already be there
    int64_t limit = platform::monotonicMs() + (timeoutMs > 0 ? timeoutMs : 0);
    slot->signal.lock();
    while (posted && !slot->answered) {
        int64_t left = limit - platform::monotonicMs();
        if (left <= 0) {
            break;
        }
        slot->signal.wait(left);
    }

    // what came, then the slot freed (a late answer finds it gone)
    hal::i2c::Status status = hal::i2c::Status::timeout;
    if (!posted) {
        status = hal::i2c::Status::noAck;
    } else if (slot->answered) {
        status = (hal::i2c::Status)slot->status;
        if (slot->read != nullptr) {
            memcpy(read, slot->read, readSize);
        }
    }

    freePending(slot);
    slot->signal.unlock();
    return status;
}

/**
 * The answer of a component: copied into the slot of that request, if it
 * still waits, then the program's thread woken.
 */
void i2cAnswer(uint32_t request, int status, const uint8_t *bytes, size_t size) {
    for (int i = 0; i < MAX_PENDING; i++) {
        Pending *slot = &pending[i];
        slot->signal.lock();
        bool waiting = slot->used && slot->request == request && !slot->answered;
        if (waiting) {
            // the bytes read: what the component gave, zeros after
            slot->status = status;
            if (status == (int)hal::i2c::Status::ok && slot->readSize > 0) {
                slot->read = (uint8_t *)memory::allocate(slot->readSize);
                memset(slot->read, 0, slot->readSize);
                memcpy(slot->read, bytes, size < slot->readSize ? size : slot->readSize);
            }

            slot->answered = true;
            slot->signal.notify();
        }

        slot->signal.unlock();
    }
}

/**
 * The first report of the twin's runtime, kept, then the agent woken.
 */
void readyReport(const char *error) {
    readySignal.lock();
    if (!readyDone) {
        readyDone = true;
        readyError = error != nullptr ? memory::copyText(error, strlen(error)) : nullptr;
        readySignal.notify();
    }

    readySignal.unlock();
}

/**
 * Waits for the report; gives its error away.
 */
bool readyWait(int64_t timeoutMs, char **error) {
    int64_t limit = platform::monotonicMs() + timeoutMs;
    readySignal.lock();
    while (!readyDone) {
        int64_t left = limit - platform::monotonicMs();
        if (left <= 0) {
            break;
        }
        readySignal.wait(left);
    }

    bool done = readyDone;
    *error = readyError;
    readyError = nullptr;
    readySignal.unlock();
    return done;
}

} // namespace twin
