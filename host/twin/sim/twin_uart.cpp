/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The UARTs of the digital twin (see twin.h, "UARTs"): the driver of
// device/serial in the twin (hal_serial.h), their TCP links (twin_tcp.h:
// the port chosen on the page, or "qjsx agent --board ... --uart-port 8093":
// UART n on 8093 + n) and their traffic for the page.
//
// Each UART keeps:
//   - a receive buffer, as the driver of a real UART: the bytes from the TCP
//     client and from the page wait there for the program's reader thread
//     (past 4 KB, the bytes are lost, said on stderr);
//   - its records: what the program wrote and what it received, numbered,
//     the last 16 KB, for the page.
//
// Threads: open / write / close / list on the JS thread of the program;
// read on its reader thread; the "twin tcp" thread gives what the TCP
// clients send; uartStates / uartTraffic / uartSend on the page's thread.
// Each UART has its own lock, which also wakes its reader.
#include "twin.h"
#include "memory.h"
#include "platform.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

// A UART as device/serial holds it (hal_serial.h): its number.
struct hal::serial::Port {
    int number;                              // the UART
};

namespace {

const size_t RECEIVE_BUFFER = 4096;          // bytes waiting for the program, at most
const size_t LOG_LIMIT = 16 * 1024;          // bytes of records kept per UART
const int READ_WAIT_MS = 100;                // a read waits that long for bytes at most
const int CLOSING_WAIT_MS = 300;             // open() waits that long for a UART being closed
const int CLOSING_POLL_MS = 10;              // ...looking that often

// A record of the traffic of a UART.
struct Record {
    uint32_t         sequence;               // its number, from 1 (per UART)
    twin::UartSource source;                 // who wrote it
    uint8_t         *bytes;                  // owned (memory::allocate)
    size_t           size;                   // bytes
    Record          *next;                   // newer record; nullptr: the last
};

// A UART of the twin.
struct Uart {
    hal::serial::Port     port;              // what device/serial holds (its number)
    platform::Signal      lock;              // all below; notified when bytes arrive or the UART closes
    bool                  link;              // the agent's link of the real board: refused
    bool                  open;              // the program has it open
    hal::serial::Settings settings;          // as opened
    twin::TcpLink         tcp;               // its TCP link (not for the agent's link)
    char                  key[8];            // name of the link: "uart<n>"
    uint8_t               received[RECEIVE_BUFFER];   // ring of bytes for the program
    size_t                receivedStart;     // first byte in 'received'
    size_t                receivedCount;     // bytes in 'received'
    Record               *first;             // oldest record; nullptr: none
    Record               *last;              // newest record
    size_t                logged;            // bytes in the records
    uint32_t              sequence;          // number of the newest record
};

Uart uartTable[twin::MAX_UARTS];

/**
 * UART 'number' of the board, or nullptr.
 */
Uart *uartAt(int number) {
    bool valid = number >= 0 && number < twin::uartCount();
    return valid ? &uartTable[number] : nullptr;
}

/**
 * Adds a record of 'size' bytes (the last LOG_LIMIT of them if more), then
 * drops the oldest records past LOG_LIMIT bytes. Lock held.
 */
void addRecord(Uart *uart, twin::UartSource source, const uint8_t *data, size_t size) {
    // the record, newest
    size_t kept = size < LOG_LIMIT ? size : LOG_LIMIT;
    Record *record = (Record *)memory::allocate(sizeof(Record));
    record->sequence = ++uart->sequence;
    record->source = source;
    record->bytes = (uint8_t *)memory::allocate(kept > 0 ? kept : 1);
    memcpy(record->bytes, data + (size - kept), kept);
    record->size = kept;
    record->next = nullptr;
    if (uart->last != nullptr) {
        uart->last->next = record;
    } else {
        uart->first = record;
    }

    uart->last = record;
    uart->logged += kept;

    // the oldest go
    while (uart->logged > LOG_LIMIT && uart->first != record) {
        Record *oldest = uart->first;
        uart->first = oldest->next;
        uart->logged -= oldest->size;
        memory::release(oldest->bytes);
        memory::release(oldest);
    }
}

/**
 * Bytes for the program: into the receive buffer (what does not fit is lost,
 * said on stderr), recorded, the reader woken. False if the program does not
 * have the UART open (dropped).
 */
bool deliver(Uart *uart, twin::UartSource source, const uint8_t *data, size_t size) {
    uart->lock.lock();
    if (!uart->open) {
        uart->lock.unlock();
        return false;
    }

    // into the ring, as much as fits
    size_t room = RECEIVE_BUFFER - uart->receivedCount;
    size_t taken = size < room ? size : room;
    for (size_t i = 0; i < taken; i++) {
        size_t at = (uart->receivedStart + uart->receivedCount + i) % RECEIVE_BUFFER;
        uart->received[at] = data[i];
    }

    uart->receivedCount += taken;

    // recorded, the reader woken
    addRecord(uart, source, data, taken);
    uart->lock.notify();
    uart->lock.unlock();

    if (taken < size) {
        fprintf(stderr, "qjsx twin: UART %d: receive buffer full, %d bytes lost\n", uart->port.number,
                (int)(size - taken));
    }

    return true;
}

/**
 * Bytes from the TCP client of a UART ("twin tcp" thread): to the program,
 * dropped when it does not have the UART open.
 */
void onTcpData(void *context, uint8_t *data, size_t size) {
    deliver((Uart *)context, twin::UartSource::tcp, data, size);
    memory::release(data);
}

/**
 * A client of a UART came or went: nothing to tell the program (a UART has
 * no line state).
 */
void onTcpClient(void *, bool) {
}

/**
 * True while the program has UART 'uart' open.
 */
bool isOpen(Uart *uart) {
    uart->lock.lock();
    bool open = uart->open;
    uart->lock.unlock();
    return open;
}

/**
 * Opens UART 'number' (see hal_serial.h for the errors): a UART of the board,
 * not the agent's link, free (a UART being closed by the reader of the
 * previous program is waited for).
 */
hal::serial::Port *openPort(const char *, int number, const hal::serial::Settings *settings, int *error) {
    // a UART of the board, not the link
    Uart *uart = uartAt(number);
    if (uart == nullptr) {
        *error = ENOENT;
        return nullptr;
    }

    if (uart->link) {
        *error = EBUSY;
        return nullptr;
    }

    // free, or freed soon
    for (int waited = 0; waited < CLOSING_WAIT_MS && isOpen(uart); waited += CLOSING_POLL_MS) {
        platform::sleepMs(CLOSING_POLL_MS);
    }

    // take it, its receive buffer empty
    uart->lock.lock();
    bool busy = uart->open;
    if (!busy) {
        uart->open = true;
        uart->settings = *settings;
        uart->receivedStart = 0;
        uart->receivedCount = 0;
    }

    uart->lock.unlock();
    if (busy) {
        *error = EBUSY;
        return nullptr;
    }

    return &uart->port;
}

/**
 * Takes what waits in the receive buffer, waiting READ_WAIT_MS at most.
 */
int readPort(hal::serial::Port *port, uint8_t *buffer, size_t size) {
    Uart *uart = &uartTable[port->number];

    // wait for bytes (or the close)
    uart->lock.lock();
    if (uart->receivedCount == 0 && uart->open) {
        uart->lock.wait(READ_WAIT_MS);
    }

    // take what is there
    size_t taken = uart->receivedCount < size ? uart->receivedCount : size;
    for (size_t i = 0; i < taken; i++) {
        buffer[i] = uart->received[(uart->receivedStart + i) % RECEIVE_BUFFER];
    }

    uart->receivedStart = (uart->receivedStart + taken) % RECEIVE_BUFFER;
    uart->receivedCount -= taken;
    uart->lock.unlock();
    return (int)taken;
}

/**
 * Records the bytes and sends them to the TCP client, if any (a send that
 * fails is ignored: the thread sees the client go).
 */
bool writePort(hal::serial::Port *port, const uint8_t *data, size_t size) {
    Uart *uart = &uartTable[port->number];

    uart->lock.lock();
    addRecord(uart, twin::UartSource::program, data, size);
    uart->lock.unlock();

    uart->tcp.send(data, size);
    return true;
}

/**
 * Frees the UART; what waits in its buffer is dropped.
 */
void closePort(hal::serial::Port *port) {
    Uart *uart = &uartTable[port->number];

    uart->lock.lock();
    uart->open = false;
    uart->receivedCount = 0;
    uart->lock.notify();
    uart->lock.unlock();
}

/**
 * The UARTs neither open nor the link.
 */
void listPorts(hal::serial::ListHandler *add, void *context) {
    for (int number = 0; number < twin::uartCount(); number++) {
        Uart *uart = &uartTable[number];
        if (!uart->link && !isOpen(uart)) {
            add(context, nullptr, number);
        }
    }
}

const hal::serial::Driver TWIN_DRIVER = {
    false,                                   // UARTs by number, with pins
    &openPort,
    &readPort,
    &writePort,
    &closePort,
    &listPorts,
};

} // namespace

namespace twin {

/**
 * The driver of device/serial in the twin.
 */
const hal::serial::Driver *serialDriver() {
    return &TWIN_DRIVER;
}

/**
 * 1. the UARTs at rest, the agent's link marked;
 * 2. a TCP link each (the agent's link excepted): listening from the start
 *    on 'port' + n when 'port' is given (not saved), else as the page
 *    chooses.
 */
void setupUarts(const char *host, int port) {
    for (int i = 0; i < uartCount(); i++) {
        // 1. at rest
        Uart *uart = &uartTable[i];
        uart->port.number = i;
        uart->link = i == linkUartNumber();
        if (uart->link) {
            continue;
        }

        // 2. its link
        bool forced = port != 0;
        snprintf(uart->key, sizeof(uart->key), "uart%d", i);
        uart->tcp.setup(uart->key, !forced, &onTcpData, &onTcpClient, uart);
        registerTcpLink(&uart->tcp);
        if (forced) {
            uart->tcp.request(port + i, true);
            printf("qjsx twin: uart %d on tcp %s:%d\n", i, host, port + i);
        }
    }
}

/**
 * Copies the states of the UARTs.
 */
void uartStates(UartState *states, int count) {
    for (int i = 0; i < count && i < uartCount(); i++) {
        Uart *uart = &uartTable[i];
        uart->lock.lock();
        states[i].link = uart->link;
        states[i].open = uart->open;
        states[i].settings = uart->settings;
        uart->lock.unlock();

        TcpState none = { 0, false, false, false, "" };
        states[i].tcp = uart->link ? none : uart->tcp.state();
    }
}

/**
 * Visits the records newer than 'after'.
 */
uint32_t uartTraffic(int number, uint32_t after, TrafficVisitor *visit, void *context) {
    Uart *uart = uartAt(number);
    if (uart == nullptr) {
        return 0;
    }

    uart->lock.lock();
    for (Record *record = uart->first; record != nullptr; record = record->next) {
        if (record->sequence > after) {
            visit(context, record->sequence, record->source, record->bytes, record->size);
        }
    }

    uint32_t newest = uart->sequence;
    uart->lock.unlock();
    return newest;
}

/**
 * The page sends bytes to the program.
 */
bool uartSend(int number, const uint8_t *bytes, size_t size) {
    Uart *uart = uartAt(number);
    if (uart == nullptr || uart->link) {
        return false;
    }

    return deliver(uart, UartSource::page, bytes, size);
}

} // namespace twin
