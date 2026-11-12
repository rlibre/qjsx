/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The TCP links of the digital twin: a program outside the twin plays the
// other end of a UART (twin_uart.cpp) or of a serial function of the USB
// device (twin_usb.cpp) through a TCP port of the PC, one client at a time.
//
// The page chooses each port and turns its listening on or off; the choices
// are kept in <storage>/twin.json and read again at the next start (nothing
// listens the first time). "qjsx agent --uart-port" / "--usb-port" make a
// group listen from the start instead, on consecutive ports, and nothing of
// that group is saved during that run.
//
// One "twin tcp" thread serves every link: it applies what was asked (a port
// that cannot be listened on turns the listening off and gives the error),
// accepts the clients and reads them. A link that is not active (the USB
// device stopped) does not listen.
//
// Threads: request / setActive / send / state on any thread; the handlers
// are called on the "twin tcp" thread, without any lock of the link held.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "platform.h"
#include "sockets.h"

namespace twin {

const int TCP_ERROR_SIZE = 64;              // bytes of TcpState.error

// What the page reads of a link.
struct TcpState {
    int  port;                              // the port chosen; 0: none yet
    bool listen;                            // listening asked
    bool listening;                         // listening now (asked, active, port free)
    bool client;                            // a client is connected
    char error[TCP_ERROR_SIZE];             // why the last listening failed; "": none
};

// Bytes the client sent ('data' from memory::allocate, owned by the handler).
typedef void TcpDataHandler(void *context, uint8_t *data, size_t size);

// A client connected (true) or went away (false).
typedef void TcpClientHandler(void *context, bool connected);

// A TCP link (see the top of the file). Static objects: set up once, before
// startTcp(), never freed.
class TcpLink {
public:
    /**
     * Names the link ('key', as in twin.json: "uart1", "usb0"; static) and
     * gives its handlers. 'persisted': its choices are saved (false for a
     * group the command line set).
     */
    void     setup(const char *key, bool persisted, TcpDataHandler *onData, TcpClientHandler *onClient,
                   void *context);

    /**
     * Asks for 'port' (1 to 65535) and listening on or off; the "twin tcp"
     * thread applies it, then saves the choices if the link is persisted.
     */
    void     request(int port, bool listen);

    /**
     * Active (the default) or not: an inactive link does not listen (its
     * client is dropped), whatever was asked.
     */
    void     setActive(bool active);

    /**
     * Sends bytes to the client; dropped without one.
     */
    void     send(const uint8_t *data, size_t size);

    /**
     * True while a client is connected.
     */
    bool     hasClient();

    /**
     * The state of the link now.
     */
    TcpState state();

    /**
     * The key of the link.
     */
    const char *key() const {
        return _key;
    }

    /**
     * True if its choices are saved.
     */
    bool     persisted() const {
        return _persisted;
    }

    /**
     * One look ("twin tcp" thread): applies what was asked, accepts a
     * client, reads it. True if the choices changed and must be saved.
     */
    bool     poll(const char *host);

private:
    /**
     * Closes the client, if any. Lock held.
     */
    void     dropClient();

    /**
     * Brings the listener in line with what is asked: closes it, or opens it
     * (a failure turns the listening off and sets the error). Lock held.
     */
    void     reconcile(const char *host);

    const char       *_key = "";             // "uart1", "usb0" (static)
    bool              _persisted = true;     // its choices are saved
    TcpDataHandler   *_onData = nullptr;     // called by the thread
    TcpClientHandler *_onClient = nullptr;   // called by the thread
    void             *_context = nullptr;    // given to the handlers
    platform::Mutex   _lock;                 // all below
    int               _port = 0;             // asked
    bool              _listen = false;       // asked
    bool              _active = true;        // the USB device is started (always for a UART)
    bool              _changed = false;      // a request not applied yet
    int               _listenedPort = 0;     // the port the listener is on
    sockets::Socket   _listener = sockets::invalidSocket;
    sockets::Socket   _client = sockets::invalidSocket;
    char              _error[TCP_ERROR_SIZE] = "";
};

/**
 * Adds a set-up link to those the "twin tcp" thread serves (before
 * startTcp()).
 */
void registerTcpLink(TcpLink *link);

/**
 * Reads the choices kept in '<storage>/twin.json' (persisted links take
 * them; the others keep them to write back) and starts the "twin tcp"
 * thread, listening on 'host'. 'storage' must live as long as the process.
 * False after printing why.
 */
bool startTcp(const char *host, const char *storage);

/**
 * The link 'key', or nullptr.
 */
TcpLink *findTcpLink(const char *key);

} // namespace twin
