/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The TCP links of the digital twin (see twin_tcp.h): each link, the
// "twin tcp" thread that serves them all, and the choices kept in
// <storage>/twin.json:
//
//   { "uart1": { "port": 8094, "listen": true }, "usb0": { ... } }
#include "twin_tcp.h"
#include "files.h"
#include "memory.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "quickjs.h"

namespace twin {

namespace {

const int MAX_LINKS = 16;                    // links the thread serves
const int POLL_MS = 10;                      // the thread looks at the sockets that often
const size_t FILE_SIZE = 2048;               // largest twin.json written

// A link served by the thread, with what twin.json holds for it.
struct Entry {
    TcpLink *link;                           // static
    bool     hasSaved;                       // twin.json has an entry for it
    int      savedPort;                      // its port there
    bool     savedListen;                    // its listening there
};

Entry entries[MAX_LINKS];                    // written before the thread starts, then by it only
int entryCount = 0;                          // entries used
const char *listenHost = "127.0.0.1";        // address of the links (set by startTcp)
char *choicesPath = nullptr;                 // <storage>/twin.json, owned

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
 * Reads the choices of twin.json into the entries (a missing or broken file
 * is no choice at all); the persisted links take them.
 */
void loadChoices() {
    // the file
    size_t size = 0;
    uint8_t *text = files::readAll(choicesPath, &size);
    if (text == nullptr) {
        return;
    }

    char *json = (char *)memory::allocate(size + 1);
    memcpy(json, text, size);
    json[size] = 0;
    memory::release(text);

    // parsed with a short-lived QuickJS context
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(runtime);
    JSValue choices = JS_ParseJSON(ctx, json, size, choicesPath);

    // one object per link
    for (int i = 0; i < entryCount && JS_IsObject(choices); i++) {
        Entry *entry = &entries[i];
        JSValue choice = JS_GetPropertyStr(ctx, choices, entry->link->key());
        if (JS_IsObject(choice)) {
            readInt(ctx, choice, "port", &entry->savedPort);
            JSValue listenValue = JS_GetPropertyStr(ctx, choice, "listen");
            entry->savedListen = JS_ToBool(ctx, listenValue) == 1;
            JS_FreeValue(ctx, listenValue);
            entry->hasSaved = true;
        }

        JS_FreeValue(ctx, choice);

        // a persisted link takes its choice
        if (entry->hasSaved && entry->link->persisted()) {
            entry->link->request(entry->savedPort, entry->savedListen);
        }
    }

    JS_FreeValue(ctx, choices);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    memory::release(json);
}

/**
 * Writes twin.json: the choices of the persisted links, and what the file
 * held for the others (a group the command line set this run).
 */
void saveChoices() {
    char *json = (char *)memory::allocate(FILE_SIZE);
    size_t used = (size_t)snprintf(json, FILE_SIZE, "{");
    bool first = true;

    for (int i = 0; i < entryCount; i++) {
        // the persisted links: what is asked now
        Entry *entry = &entries[i];
        if (entry->link->persisted()) {
            TcpState state = entry->link->state();
            entry->savedPort = state.port;
            entry->savedListen = state.listen;
            entry->hasSaved = state.port != 0;
        }

        if (!entry->hasSaved || used >= FILE_SIZE) {
            continue;
        }

        // "key": { "port": n, "listen": b }
        used += (size_t)snprintf(json + used, FILE_SIZE - used, "%s\n    \"%s\": { \"port\": %d, \"listen\": %s }",
                                 first ? "" : ",", entry->link->key(), entry->savedPort,
                                 entry->savedListen ? "true" : "false");
        first = false;
    }

    if (used < FILE_SIZE) {
        used += (size_t)snprintf(json + used, FILE_SIZE - used, "\n}\n");
    }

    // a failure is only said: the twin goes on
    size_t length = strlen(json);
    if (!files::writeAll(choicesPath, json, length)) {
        fprintf(stderr, "qjsx twin: cannot write %s\n", choicesPath);
    }

    memory::release(json);
}

/**
 * The "twin tcp" thread: one look at every link, then twin.json written if
 * a choice changed; for the life of the process.
 */
void serveLinks(void *) {
    for (;;) {
        bool changed = false;
        for (int i = 0; i < entryCount; i++) {
            changed = entries[i].link->poll(listenHost) || changed;
        }

        if (changed) {
            saveChoices();
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
    if (entryCount < MAX_LINKS) {
        entries[entryCount++] = Entry{ link, false, 0, false };
    }
}

/**
 * Reads twin.json, then starts the thread.
 */
bool startTcp(const char *host, const char *storage) {
    // the choices kept
    listenHost = host;
    size_t size = strlen(storage) + strlen("/twin.json") + 1;
    choicesPath = (char *)memory::allocate(size);
    snprintf(choicesPath, size, "%s/twin.json", storage);
    loadChoices();

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
    for (int i = 0; i < entryCount; i++) {
        if (strcmp(entries[i].link->key(), key) == 0) {
            return entries[i].link;
        }
    }

    return nullptr;
}

} // namespace twin
