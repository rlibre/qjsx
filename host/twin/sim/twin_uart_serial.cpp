/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The UARTs of the twin wired to real serial ports of the PC (see twin.h,
// "UARTs wired to PC serial ports"), through the PC's serial driver
// (host/platform/serial_host.cpp), the one device/serial uses outside a twin.
//
// Each UART but the agent's link has a wiring and two threads, started once:
//   - "twin serial <n>" owns the PC port: it opens it when the wiring is
//     asked and the program has the UART open (with the program's
//     settings), closes it when that is no longer so or the settings
//     changed, and reads it (the driver returns every 100 ms at most): what
//     comes goes to the program;
//   - "twin serial <n> out" sends what the program wrote in "immediate"
//     mode, from a queue.
// At real speed the program's thread writes itself, then waits for the time
// the bytes take on the line (the driver returns as soon as the system took
// them, long before).
//
// Locks: 'lock' protects the wiring, the state of the UART and the queue;
// 'portLock' the use of the open port by the writers, so that the owning
// thread closes it only between two writes. Order: portLock, then lock.
// The choice is kept in twin.json under "serial<n>": { port, connect,
// realSpeed }, unless the wiring is fixed (command line, qjsx.json).
#include "twin.h"
#include "memory.h"
#include "platform.h"
#include "serial_host.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

namespace twin {

namespace {

const size_t READ_SIZE = 256;            // bytes read from the PC port at a time
const size_t QUEUE_LIMIT = 64 * 1024;    // bytes waiting to be sent in "immediate" mode, at most
const int IDLE_MS = 50;                  // the owning thread looks that often while the port is closed
const int QUEUE_WAIT_MS = 1000;          // the sending thread wakes that often without bytes
const size_t CHOICE_SIZE = 2 * SERIAL_NAME_SIZE + 96;   // largest JSON text of a choice

// The wiring of a UART to a PC serial port.
struct SerialLink {
    int                    number;            // the UART
    char                   key[16];           // its key in twin.json: "serial<n>"
    platform::Signal       lock;              // below, but 'open' read by its owner; notified when bytes are queued
    char                   port[SERIAL_NAME_SIZE];   // the port asked; "": none
    bool                   connect;           // wiring asked
    bool                   realSpeed;         // the program's writes take the time of the line
    bool                   fixed;             // set by the command line or qjsx.json: not saved
    bool                   uartOpen;          // the program has the UART open
    hal::serial::Settings  settings;          // as the program opened it
    uint32_t               generation;        // changes when the port or the settings change: reopen
    char                   error[SERIAL_ERROR_SIZE];   // why the last opening failed
    platform::Mutex        portLock;          // the use of 'open' by the writers, its close
    hal::serial::Port     *open;              // the PC port; changed under portLock and lock by the owner only
    uint32_t               openGeneration;    // 'generation' when it was opened (owner only)
    uint8_t               *queue;             // "immediate" mode: bytes to send, QUEUE_LIMIT (owned)
    size_t                 queued;            // bytes in 'queue'
    bool                   overflowSaid;      // the loss of bytes was said on stderr
};

SerialLink links[MAX_UARTS];                  // one per UART (the agent's link unused)

/**
 * The PC's serial driver.
 */
const hal::serial::Driver *pcDriver() {
    return host::serialDriver();
}

/**
 * The wiring of UART 'number', or nullptr (no such UART, the agent's link).
 */
SerialLink *linkAt(int number) {
    bool valid = number >= 0 && number < uartCount() && number != linkUartNumber();
    return valid ? &links[number] : nullptr;
}

/**
 * Copies 'text' into 'out' (a string of 'size' bytes), cut if longer.
 */
void copyName(char *out, size_t size, const char *text) {
    snprintf(out, size, "%s", text != nullptr ? text : "");
}

/**
 * 'text' as a JSON string into 'out' (quotes and backslashes escaped).
 */
void jsonString(char *out, size_t size, const char *text) {
    size_t used = 0;
    out[used++] = '"';
    for (const char *c = text; *c != 0 && used + 3 < size; c++) {
        if (*c == '"' || *c == '\\') {
            out[used++] = '\\';
        }

        out[used++] = *c;
    }

    out[used++] = '"';
    out[used] = 0;
}

/**
 * Keeps the choice of a link in twin.json, unless it is fixed.
 */
void saveChoice(SerialLink *link) {
    // what is asked now
    link->lock.lock();
    bool fixed = link->fixed;
    char port[2 * SERIAL_NAME_SIZE + 2];
    jsonString(port, sizeof(port), link->port);
    bool connect = link->connect;
    bool realSpeed = link->realSpeed;
    link->lock.unlock();
    if (fixed) {
        return;
    }

    // { "port": "COM5", "connect": true, "realSpeed": true }
    char json[CHOICE_SIZE];
    snprintf(json, sizeof(json), "{ \"port\": %s, \"connect\": %s, \"realSpeed\": %s }", port, connect ? "true" : "false",
             realSpeed ? "true" : "false");
    choicesWrite(link->key, json);
}

/**
 * The choice kept for a link ({ port, connect, realSpeed }): the link takes
 * it (real speed when it is missing).
 */
void takeChoice(void *context, JSContext *ctx, JSValueConst choice) {
    SerialLink *link = (SerialLink *)context;

    // the port
    JSValue portValue = JS_GetPropertyStr(ctx, choice, "port");
    const char *port = JS_IsString(portValue) ? JS_ToCString(ctx, portValue) : nullptr;
    copyName(link->port, sizeof(link->port), port);
    JS_FreeCString(ctx, port);
    JS_FreeValue(ctx, portValue);

    // the wiring and the speed
    JSValue connectValue = JS_GetPropertyStr(ctx, choice, "connect");
    link->connect = JS_ToBool(ctx, connectValue) == 1;
    JS_FreeValue(ctx, connectValue);

    JSValue speedValue = JS_GetPropertyStr(ctx, choice, "realSpeed");
    link->realSpeed = JS_IsUndefined(speedValue) || JS_ToBool(ctx, speedValue) == 1;
    JS_FreeValue(ctx, speedValue);
}

/**
 * The text of an error of the driver's open().
 */
const char *openError(int error) {
    switch (error) {
        case ENOENT: {
            return "no such port";
        }
        case EBUSY: {
            return "port busy (open elsewhere)";
        }
        case EINVAL: {
            return "settings refused by the port";
        }
        default: {
            return "cannot open the port";
        }
    }
}

/**
 * The settings of the PC port from those of the UART: the same line, flow
 * control when the program asked for it or wired RTS and CTS, no pins.
 */
hal::serial::Settings pcSettings(const hal::serial::Settings *uart) {
    hal::serial::Settings settings = *uart;
    bool wiredFlowControl = uart->rts != hal::serial::NO_PIN && uart->cts != hal::serial::NO_PIN;
    settings.flowControl = uart->flowControl || wiredFlowControl;
    settings.tx = hal::serial::NO_PIN;
    settings.rx = hal::serial::NO_PIN;
    settings.rts = hal::serial::NO_PIN;
    settings.cts = hal::serial::NO_PIN;
    return settings;
}

/**
 * Milliseconds that 'size' bytes take on the line with 'settings': a start
 * bit, the data bits, the parity bit, the stop bits each.
 */
int64_t lineMs(const hal::serial::Settings *settings, size_t size) {
    int parityBits = settings->parity == hal::serial::Parity::none ? 0 : 1;
    int64_t bitsPerByte = 1 + settings->dataBits + parityBits + settings->stopBits;
    int64_t baudRate = settings->baudRate > 0 ? settings->baudRate : 1;
    return (int64_t)size * bitsPerByte * 1000 / baudRate;
}

/**
 * Closes the PC port of a link (owning thread), between two writes.
 */
void closePcPort(SerialLink *link) {
    link->portLock.lock();
    link->lock.lock();
    hal::serial::Port *port = link->open;
    link->open = nullptr;
    link->queued = 0;
    link->lock.unlock();
    link->portLock.unlock();

    if (port != nullptr) {
        pcDriver()->close(port);
    }
}

/**
 * Turns the wiring off after a failure, says why, keeps the choice (owning
 * thread).
 */
void failWiring(SerialLink *link, const char *why) {
    link->lock.lock();
    link->connect = false;
    copyName(link->error, sizeof(link->error), why);
    link->lock.unlock();
    saveChoice(link);
}

/**
 * Brings the PC port in line with what is asked (owning thread):
 * 1. what is wanted, under the lock;
 * 2. an open port no longer wanted, or opened for another port or other
 *    settings, is closed;
 * 3. a wanted port is opened; a failure turns the wiring off and says why.
 */
void reconcile(SerialLink *link) {
    // 1. what is wanted
    link->lock.lock();
    bool wanted = link->connect && link->uartOpen && link->port[0] != 0;
    uint32_t generation = link->generation;
    bool stale = link->open != nullptr && (!wanted || generation != link->openGeneration);
    char name[SERIAL_NAME_SIZE];
    copyName(name, sizeof(name), link->port);
    hal::serial::Settings settings = pcSettings(&link->settings);
    link->lock.unlock();

    // 2. close
    if (stale) {
        closePcPort(link);
    }

    if (!wanted || link->open != nullptr) {
        return;
    }

    // 3. open
    int error = 0;
    hal::serial::Port *port = pcDriver()->open(name, -1, &settings, &error);
    if (port == nullptr) {
        failWiring(link, openError(error));
        return;
    }

    link->portLock.lock();
    link->lock.lock();
    link->open = port;
    link->openGeneration = generation;
    link->error[0] = 0;
    link->lock.unlock();
    link->portLock.unlock();
}

/**
 * The owning thread of a link: reconcile, then read the open port (what
 * comes goes to the program); a read that fails closes the port and turns
 * the wiring off. For the life of the process.
 */
void ownPort(void *argument) {
    SerialLink *link = (SerialLink *)argument;
    uint8_t *buffer = (uint8_t *)memory::allocate(READ_SIZE);

    for (;;) {
        // the port as asked
        reconcile(link);
        hal::serial::Port *port = link->open;
        if (port == nullptr) {
            platform::sleepMs(IDLE_MS);
            continue;
        }

        // what it received, to the program
        int received = pcDriver()->read(port, buffer, READ_SIZE);
        if (received > 0) {
            uartReceive(link->number, UartSource::serial, buffer, (size_t)received);
        } else if (received < 0) {
            closePcPort(link);
            failWiring(link, "read failed (port removed?)");
        }
    }
}

/**
 * The sending thread of a link ("immediate" mode): waits for queued bytes,
 * takes them all, writes them to the open port. For the life of the
 * process.
 */
void sendQueue(void *argument) {
    SerialLink *link = (SerialLink *)argument;
    uint8_t *sending = (uint8_t *)memory::allocate(QUEUE_LIMIT);

    for (;;) {
        // the queued bytes, all of them
        link->lock.lock();
        if (link->queued == 0) {
            link->lock.wait(QUEUE_WAIT_MS);
        }

        size_t size = link->queued;
        memcpy(sending, link->queue, size);
        link->queued = 0;
        link->lock.unlock();
        if (size == 0) {
            continue;
        }

        // to the port, if it is still open (a failure is seen by the reader)
        link->portLock.lock();
        if (link->open != nullptr) {
            pcDriver()->write(link->open, sending, size);
        }

        link->portLock.unlock();
    }
}

} // namespace

/**
 * Records the fixed wiring.
 */
void serialFix(int uart, const char *port, bool realSpeed) {
    SerialLink *link = &links[uart];
    copyName(link->port, sizeof(link->port), port);
    link->connect = true;
    link->realSpeed = realSpeed;
    link->fixed = true;
}

/**
 * Each UART but the agent's link:
 * 1. its key, its queue, the choice kept (unless fixed);
 * 2. its two threads.
 */
bool setupSerial() {
    for (int i = 0; i < uartCount(); i++) {
        SerialLink *link = linkAt(i);
        if (link == nullptr) {
            continue;
        }

        // 1. the link, as kept or fixed
        link->number = i;
        snprintf(link->key, sizeof(link->key), "serial%d", i);
        link->queue = (uint8_t *)memory::allocate(QUEUE_LIMIT);
        if (link->fixed) {
            printf("qjsx twin: uart %d wired to %s\n", i, link->port);
        } else {
            link->realSpeed = true;
            choicesRead(link->key, &takeChoice, link);
        }

        // 2. its threads
        char ownerName[24];
        snprintf(ownerName, sizeof(ownerName), "twin serial %d", i);
        char senderName[24];
        snprintf(senderName, sizeof(senderName), "twin serial %d out", i);
        bool started = platform::startThread(&ownPort, link, platform::defaultThreadOptions(ownerName))
                    && platform::startThread(&sendQueue, link, platform::defaultThreadOptions(senderName));
        if (!started) {
            fprintf(stderr, "qjsx twin: cannot start the threads of the serial wiring of uart %d\n", i);
            return false;
        }
    }

    return true;
}

/**
 * Takes what the page asks; a new port or a new wiring opens the port again.
 */
void serialRequest(int uart, const char *port, bool connect, bool realSpeed) {
    SerialLink *link = linkAt(uart);
    if (link == nullptr) {
        return;
    }

    link->lock.lock();
    bool newPort = strcmp(link->port, port) != 0 || link->connect != connect;
    if (newPort) {
        link->generation++;
    }

    copyName(link->port, sizeof(link->port), port);
    link->connect = connect;
    link->realSpeed = realSpeed;
    if (connect) {
        link->error[0] = 0;
    }

    link->lock.unlock();
    saveChoice(link);
}

/**
 * The state of the wiring.
 */
void serialState(int uart, SerialState *state) {
    *state = SerialState{};
    SerialLink *link = linkAt(uart);
    if (link == nullptr) {
        return;
    }

    link->lock.lock();
    copyName(state->port, sizeof(state->port), link->port);
    state->connect = link->connect;
    state->realSpeed = link->realSpeed;
    state->connected = link->open != nullptr;
    state->fixed = link->fixed;
    copyName(state->error, sizeof(state->error), link->error);
    link->lock.unlock();
}

/**
 * The UART is open with these settings: a wired port opens again with them.
 */
void serialUartOpened(int uart, const hal::serial::Settings *settings) {
    SerialLink *link = linkAt(uart);
    if (link == nullptr) {
        return;
    }

    link->lock.lock();
    link->uartOpen = true;
    link->settings = *settings;
    link->generation++;
    link->lock.unlock();
}

/**
 * The UART is closed: the owning thread closes the port.
 */
void serialUartClosed(int uart) {
    SerialLink *link = linkAt(uart);
    if (link == nullptr) {
        return;
    }

    link->lock.lock();
    link->uartOpen = false;
    link->queued = 0;
    link->lock.unlock();
}

/**
 * At real speed: writes, then waits for the time the bytes take on the line
 * (less what the write took). Otherwise: queues them for the sending thread
 * (what does not fit is lost, said once). Nothing while the port is closed.
 */
void serialUartWrite(int uart, const uint8_t *data, size_t size) {
    SerialLink *link = linkAt(uart);
    if (link == nullptr) {
        return;
    }

    link->lock.lock();
    bool realSpeed = link->realSpeed;
    hal::serial::Settings settings = link->settings;
    link->lock.unlock();

    // real speed: the program's thread writes, then waits
    if (realSpeed) {
        int64_t start = platform::monotonicMs();
        link->portLock.lock();
        bool written = link->open != nullptr && pcDriver()->write(link->open, data, size);
        link->portLock.unlock();
        if (!written) {
            return;
        }

        int64_t left = lineMs(&settings, size) - (platform::monotonicMs() - start);
        if (left > 0) {
            platform::sleepMs((int)left);
        }

        return;
    }

    // immediate: into the queue, while the port is open
    link->lock.lock();
    size_t room = link->open != nullptr ? QUEUE_LIMIT - link->queued : 0;
    size_t taken = size < room ? size : room;
    memcpy(link->queue + link->queued, data, taken);
    link->queued += taken;
    bool lost = link->open != nullptr && taken < size && !link->overflowSaid;
    link->overflowSaid = link->overflowSaid || lost;
    link->lock.notify();
    link->lock.unlock();

    if (lost) {
        fprintf(stderr, "qjsx twin: UART %d: the PC port does not keep up, bytes lost\n", uart);
    }
}

/**
 * The PC's serial ports.
 */
void serialPorts(hal::serial::ListHandler *add, void *context) {
    pcDriver()->list(add, context);
}

} // namespace twin
