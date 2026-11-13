/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The TCP links of the digital twin (see twin_tcp.h): each link, the
// "twin tcp" thread that serves them all, and their choices, kept in
// <storage>/twin.json through twin_choices.cpp, one key per link:
//
//   "uart1": { "port": 8094, "listen": true }
#include "twin_tcp.h"
#include "twin.h"
#include "memory.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "quickjs.h"

namespace twin {

namespace {

const int MAX_LINKS = 16;                    // links the thread serves
const int POLL_MS = 10;                      // the thread looks at the sockets that often
const size_t CHOICE_SIZE = 64;               // largest JSON text of the choice of a link

TcpLink *links[MAX_LINKS];                   // written before the thread starts, then read by it only
int linkCount = 0;                           // entries used
const char *listenHost = "127.0.0.1";        // address of the links (set by startTcp)

/**
 * Reads the integer property 'name' of 'object' into *value (unchanged if
 * missing).
 */
void readInt(JSContext *ctx, JSValueConst object, const char *name, int *value) {
    JSValue property = JS_GetPropertyStr(ctx, object, name);
    if (JS_IsNumber(property)) {
        int32_t number = 0;
        JS_ToInt32(ctx, &number, property);
        *value = (int)number;
    }

    JS_FreeValue(ctx, property);
}

/**
 * The choice kept for a link ({ port, listen }): the link takes it.
 */
void takeChoice(void *context, JSContext *ctx, JSValueConst choice) {
    TcpLink *link = (TcpLink *)context;

    int port = 0;
    readInt(ctx, choice, "port", &port);
    JSValue listenValue = JS_GetPropertyStr(ctx, choice, "listen");
    bool listen = JS_ToBool(ctx, listenValue) == 1;
    JS_FreeValue(ctx, listenValue);

    link->request(port, listen);
}

/**
 * Keeps what is asked of a persisted link (nothing while no port was
 * chosen).
 */
void saveChoice(TcpLink *link) {
    TcpState state = link->state();
    if (state.port == 0) {
        return;
    }

    char json[CHOICE_SIZE];
    snprintf(json, sizeof(json), "{ \"port\": %d, \"listen\": %s }", state.port, state.listen ? "true" : "false");
    choicesWrite(link->key(), json);
}

/**
 * The "twin tcp" thread: one look at every link, a persisted link whose
 * choice changed saved; for the life of the process.
 */
void serveLinks(void *) {
    for (;;) {
        for (int i = 0; i < linkCount; i++) {
            bool changed = links[i]->poll(listenHost);
            if (changed && links[i]->persisted()) {
                saveChoice(links[i]);
            }
        }

        platform::sleepMs(POLL_MS);
    }
}

} // namespace

// ### TcpLink ################################################################

/**
 * Names the link and gives its handlers.
 */
void TcpLink::setup(const char *key, bool persisted, TcpDataHandler *onData, TcpClientHandler *onClient,
                    void *context) {
    _key = key;
    _persisted = persisted;
    _onData = onData;
    _onClient = onClient;
    _context = context;
}

/**
 * Records what is asked; the thread applies it.
 */
void TcpLink::request(int port, bool listen) {
    _lock.lock();
    _port = port;
    _listen = listen;
    _changed = true;
    _lock.unlock();
}

/**
 * Active or not; the thread opens or closes the listener.
 */
void TcpLink::setActive(bool active) {
    _lock.lock();
    _active = active;
    _lock.unlock();
}

/**
 * Sends to the client, if any (a send that fails is ignored: the thread
 * sees the client go).
 */
void TcpLink::send(const uint8_t *data, size_t size) {
    _lock.lock();
    if (_client != sockets::invalidSocket) {
        int error = 0;
        sockets::sendAll(_client, data, size, &error);
    }

    _lock.unlock();
}

/**
 * A client is connected.
 */
bool TcpLink::hasClient() {
    _lock.lock();
    bool connected = _client != sockets::invalidSocket;
    _lock.unlock();
    return connected;
}

/**
 * The state now.
 */
TcpState TcpLink::state() {
    TcpState state;
    _lock.lock();
    state.port = _port;
    state.listen = _listen;
    state.listening = _listener != sockets::invalidSocket;
    state.client = _client != sockets::invalidSocket;
    snprintf(state.error, sizeof(state.error), "%s", _error);
    _lock.unlock();
    return state;
}

/**
 * Closes the client. Lock held.
 */
void TcpLink::dropClient() {
    if (_client != sockets::invalidSocket) {
        sockets::closeSocket(_client);
        _client = sockets::invalidSocket;
    }
}

/**
 * 1. a listener that should not be there (listening off, link inactive,
 *    another port asked) is closed, with its client;
 * 2. a listener that should be there is opened; a failure turns the
 *    listening off and says why.
 * Lock held.
 */
void TcpLink::reconcile(const char *host) {
    bool wanted = _listen && _active;

    // 1. close
    bool open = _listener != sockets::invalidSocket;
    if (open && (!wanted || _listenedPort != _port)) {
        dropClient();
        sockets::closeSocket(_listener);
        _listener = sockets::invalidSocket;
        open = false;
    }

    if (!wanted || open) {
        return;
    }

    // 2. open
    if (_port < 1 || _port > 65535) {
        snprintf(_error, sizeof(_error), "no port chosen");
        _listen = false;
        return;
    }

    int bound = 0;
    int error = 0;
    _listener = sockets::listenOn(host, _port, &bound, &error);
    if (_listener == sockets::invalidSocket) {
        if (error == EADDRINUSE) {
            snprintf(_error, sizeof(_error), "port %d in use", _port);
        } else {
            snprintf(_error, sizeof(_error), "cannot listen on port %d: %s", _port, strerror(error));
        }

        _listen = false;
        return;
    }

    _listenedPort = _port;
    _error[0] = 0;
}

/**
 * 1. applies what was asked (a failure to listen changes the choice too);
 * 2. without a client, accepts one if it is waiting;
 * 3. with one, reads what it sent, or sees it go;
 * 4. calls the handlers, without the lock.
 */
bool TcpLink::poll(const char *host) {
    _lock.lock();

    // 1. what was asked; the client dropped with its listener counts as gone
    bool hadClient = _client != sockets::invalidSocket;
    bool listenBefore = _listen;
    bool changed = _changed;
    _changed = false;
    reconcile(host);
    changed = changed || _listen != listenBefore;
    bool dropped = hadClient && _client == sockets::invalidSocket;

    // 2. a new client
    bool connected = false;
    uint8_t *data = nullptr;
    int received = 0;
    bool listening = _listener != sockets::invalidSocket;
    if (listening && _client == sockets::invalidSocket) {
        if (sockets::waitReadable(_listener, 0) > 0) {
            int error = 0;
            _client = sockets::acceptConnection(_listener, &error);
            connected = _client != sockets::invalidSocket;
        }
    } else if (_client != sockets::invalidSocket && sockets::waitReadable(_client, 0) > 0) {
        // 3. what the client sent, or its end
        received = sockets::receiveBlock(_client, &data);
        if (received <= 0) {
            dropClient();
            dropped = true;
        }
    }

    _lock.unlock();

    // 4. the handlers
    if (received > 0) {
        _onData(_context, data, (size_t)received);
    }

    if (connected || dropped) {
        _onClient(_context, connected);
    }

    return changed;
}

// ### The links together #####################################################

/**
 * Adds a link to those the thread serves.
 */
void registerTcpLink(TcpLink *link) {
    if (linkCount < MAX_LINKS) {
        links[linkCount++] = link;
    }
}

/**
 * The persisted links take their kept choices, then the thread starts.
 */
bool startTcp(const char *host) {
    // the choices kept
    listenHost = host;
    for (int i = 0; i < linkCount; i++) {
        if (links[i]->persisted()) {
            choicesRead(links[i]->key(), &takeChoice, links[i]);
        }
    }

    // the thread
    if (!platform::startThread(&serveLinks, nullptr, platform::defaultThreadOptions("twin tcp"))) {
        fprintf(stderr, "qjsx twin: cannot start the tcp thread\n");
        return false;
    }

    return true;
}

/**
 * The link 'key'.
 */
TcpLink *findTcpLink(const char *key) {
    for (int i = 0; i < linkCount; i++) {
        if (strcmp(links[i]->key(), key) == 0) {
            return links[i];
        }
    }

    return nullptr;
}

} // namespace twin
