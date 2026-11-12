/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// hal::usb of the digital twin (see twin.h): the PC cannot be a USB device,
// so each serial function is a TCP link of the PC (twin_tcp.h: the port
// chosen in the board view of the page, or "qjsx agent --board ...
// --usb-port 8091": function 0 on 8091, 1 on 8092). A link listens only
// while the program has the device started, as a USB plug the host sees
// only then. A client that connects plays the host opening the port (DTR
// and RTS at 1); its disconnection, the host closing it. The bytes go both
// ways as they are.
//
// Threads: start / stop / write / lineState on the JS thread; the "twin tcp"
// thread calls the handlers; usbState on the page's thread. 'lock' protects
// the device.
#include "hal_usb.h"
#include "twin.h"
#include "memory.h"
#include "platform.h"

#include <stdio.h>

namespace {

// A serial function of the device: its TCP link.
struct Function {
    int           index;                     // its number
    twin::TcpLink tcp;                       // its link
    char          key[8];                    // name of the link: "usb<n>"
};

Function functions[hal::usb::MAX_SERIALS];   // the links set up by setupUsb (static)
platform::Mutex lock;                        // all below
bool running = false;                        // started
int functionCount = 0;                       // functions of the started device
hal::usb::Descriptors descriptors;           // of the started device
hal::usb::DataHandler *dataHandler = nullptr;
hal::usb::LineStateHandler *lineStateHandler = nullptr;

/**
 * Bytes from the TCP client of a function ("twin tcp" thread): to the data
 * handler while the device runs with that function, else dropped.
 */
void onTcpData(void *context, uint8_t *data, size_t size) {
    Function *function = (Function *)context;

    // the handler, if the function is part of the running device
    lock.lock();
    bool part = running && function->index < functionCount;
    hal::usb::DataHandler *onData = part ? dataHandler : nullptr;
    lock.unlock();

    if (onData != nullptr) {
        onData(function->index, data, size);
    } else {
        memory::release(data);
    }
}

/**
 * A client of a function came or went: the host opened or closed the port
 * (DTR and RTS follow).
 */
void onTcpClient(void *context, bool connected) {
    Function *function = (Function *)context;

    lock.lock();
    bool part = running && function->index < functionCount;
    hal::usb::LineStateHandler *onLine = part ? lineStateHandler : nullptr;
    lock.unlock();

    if (onLine != nullptr) {
        onLine(function->index, connected, connected);
    }
}

} // namespace

namespace twin {

/**
 * The TCP links of the serial functions: inactive until start(); listening
 * from the start on 'port' + n when 'port' is given (not saved), else as the
 * page chooses.
 */
void setupUsb(const char *host, int port) {
    bool forced = port != 0;
    for (int i = 0; i < hal::usb::MAX_SERIALS; i++) {
        Function *function = &functions[i];
        function->index = i;
        snprintf(function->key, sizeof(function->key), "usb%d", i);
        function->tcp.setup(function->key, !forced, &onTcpData, &onTcpClient, function);
        function->tcp.setActive(false);
        registerTcpLink(&function->tcp);
        if (forced) {
            function->tcp.request(port + i, true);
            printf("qjsx twin: usb serial %d on tcp %s:%d once started\n", i, host, port + i);
        }
    }
}

/**
 * The state of the USB device for the page.
 */
void usbState(UsbState *state) {
    lock.lock();
    state->started = running;
    state->descriptors = descriptors;
    state->serials = running ? functionCount : 0;
    lock.unlock();

    for (int i = 0; i < hal::usb::MAX_SERIALS; i++) {
        state->tcp[i] = functions[i].tcp.state();
    }
}

} // namespace twin

namespace hal {
namespace usb {

/**
 * The handlers of the events.
 */
void setHandlers(DataHandler *data, LineStateHandler *lineState) {
    lock.lock();
    dataHandler = data;
    lineStateHandler = lineState;
    lock.unlock();
}

/**
 * Starts the device: the links of its functions become active.
 */
bool start(const Descriptors *started, int serials) {
    if (serials < 1 || serials > MAX_SERIALS) {
        return false;
    }

    // the device
    lock.lock();
    bool busy = running;
    if (!busy) {
        running = true;
        functionCount = serials;
        descriptors = *started;
    }

    lock.unlock();
    if (busy) {
        return false;
    }

    // its functions reachable
    for (int i = 0; i < serials; i++) {
        functions[i].tcp.setActive(true);
    }

    printf("qjsx twin: usb device \"%s\" / \"%s\" started\n", started->manufacturer, started->product);
    return true;
}

/**
 * Stops the device: its links stop listening, their clients are dropped.
 */
void stop() {
    lock.lock();
    running = false;
    functionCount = 0;
    lock.unlock();

    for (int i = 0; i < MAX_SERIALS; i++) {
        functions[i].tcp.setActive(false);
    }
}

/**
 * Sends bytes to the client of the function; dropped without one.
 */
bool write(int serial, const uint8_t *data, size_t size) {
    lock.lock();
    bool valid = running && serial >= 0 && serial < functionCount;
    lock.unlock();

    if (valid) {
        functions[serial].tcp.send(data, size);
    }

    return valid;
}

/**
 * DTR and RTS: at 1 while a client is connected.
 */
void lineState(int serial, bool *dtr, bool *rts) {
    lock.lock();
    bool valid = running && serial >= 0 && serial < functionCount;
    lock.unlock();

    bool connected = valid && functions[serial].tcp.hasClient();
    *dtr = connected;
    *rts = connected;
}

} // namespace usb
} // namespace hal
