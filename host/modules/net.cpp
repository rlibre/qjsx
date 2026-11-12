/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "sys/net": TCP client and server.
//
//   import { connect, listen } from "sys/net";
//   const server = listen({ port: 0 }, connection => {
//       connection.onData(bytes => connection.write(bytes));   // echo
//   });
//   const client = await connect({ host: "127.0.0.1", port: server.port() });
//   client.onData(bytes => ...);
//   client.onClose(() => ...);
//   client.write("hello");          // string (UTF-8) or bytes
//   client.close();
//
// Each connection has a reader thread that posts the received bytes to the
// event loop; writes are synchronous. An open connection or server keeps the
// event loop running (js::keepAlive) until it is closed.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsbind.h"
#include "memory.h"
#include "loop.h"
#include "permissions.h"
#include "sockets.h"

namespace {

enum MessageKind {
    MESSAGE_DATA = 1,
    MESSAGE_CLOSED,
    MESSAGE_ACCEPTED,
    MESSAGE_CONNECTED,
    MESSAGE_FAILED,
};

// How often a producer thread checks the 'closing' flag while it waits (ms).
const int CLOSING_CHECK_MS = 100;

// A socket shared by the JS thread (Connection or Server) and its producer
// thread (reader or accept loop). Allocated by start(), freed by the last of
// the two owners: the socket is closed only once neither uses it, so the
// thread never calls recv() / accept() on a closed descriptor whose number
// the system may already have given to another file.
struct SocketState {
    sockets::Socket socket;   // owned: closed by the last owner (releaseSocketState)
    int closing;              // atomic; set by the JS side: the thread stops
    int references;           // atomic; 2 owners: the JS object and the thread
    rt::Mailbox *mailbox;     // retained until the state is freed
    rt::SourceId source;      // message source the thread posts to
};

/**
 * JS thread: the state shared by a JS object and its producer thread for
 * 'socket', posting to the current loop. Two references: each owner lets
 * its own go with releaseSocketState().
 */
SocketState *newSocketState(sockets::Socket socket, rt::SourceId source) {
    // the mailbox is retained: the thread may outlive the loop
    SocketState *state = (SocketState *)memory::allocate(sizeof(SocketState));
    *state = SocketState{ socket, 0, 2, rt::Loop::current()->mailbox(), source };
    state->mailbox->retain();
    return state;
}

/**
 * One owner of the state lets it go; the last one closes the socket, releases
 * the mailbox and frees the state. Any thread.
 */
void releaseSocketState(SocketState *state) {
    if (platform::atomicDecrement(&state->references) > 0) {
        return;
    }

    sockets::closeSocket(state->socket);
    state->mailbox->release();
    memory::release(state);
}

/**
 * JS thread: asks the producer thread to stop, then lets the JS side's share
 * go (the thread, if still running, closes the socket when it stops).
 */
void requestClose(SocketState *state) {
    // the flag is seen within CLOSING_CHECK_MS; the shutdown wakes a blocked
    // recv() at once, and tells the peer (the socket is still open: this side
    // holds a reference)
    __atomic_store_n(&state->closing, 1, __ATOMIC_RELEASE);
    sockets::shutdownSocket(state->socket);
    releaseSocketState(state);
}

/**
 * Producer thread: waits until the socket is readable or the JS side asks to
 * stop. Returns 1 when readable, 0 when asked to stop, < 0 on an error.
 */
int waitReadableOrClosing(SocketState *state) {
    for (;;) {
        // the JS side asked to stop
        if (__atomic_load_n(&state->closing, __ATOMIC_ACQUIRE)) {
            return 0;
        }

        // wait a little for the socket, then check the flag again
        int ready = sockets::waitReadable(state->socket, CLOSING_CHECK_MS);
        if (ready != 0) {
            return ready;
        }
    }
}

/**
 * Reader thread of a connection: posts the received bytes until the stream
 * ends or the JS side closes it. Owns one reference of 'argument', a
 * SocketState.
 */
void readLoop(void *argument) {
    SocketState *state = (SocketState *)argument;
    int error = 0;

    for (;;) {
        // wait for bytes; stop when the JS side closes the connection, or on
        // an error
        int ready = waitReadableOrClosing(state);
        if (ready <= 0) {
            error = ready < 0 ? sockets::lastError() : 0;
            break;
        }

        // receive straight into the block given to the loop (no buffer on the
        // stack, no copy); stop at the end of the stream (0) or on an error
        uint8_t *data = nullptr;
        int received = sockets::receiveBlock(state->socket, &data);
        if (received <= 0) {
            error = received < 0 ? sockets::lastError() : 0;
            break;
        }

        // post the block as MESSAGE_DATA: the loop owns and frees it
        state->mailbox->post(state->source, MESSAGE_DATA, data, received, 0);
    }

    // tell the JS thread (errno-style error, 0 for a normal end), then let the
    // state go (the last owner closes the socket: the JS side may still be
    // writing)
    state->mailbox->post(state->source, MESSAGE_CLOSED, nullptr, 0, error);
    releaseSocketState(state);
}

/**
 * Accept thread of a server: posts each accepted socket until the JS side
 * closes the server (or accept fails). Owns one reference of 'argument', a
 * SocketState.
 */
void acceptLoop(void *argument) {
    SocketState *state = (SocketState *)argument;

    for (;;) {
        // wait for a connection; stop when the JS side closes the server, or
        // on an error
        int ready = waitReadableOrClosing(state);
        if (ready <= 0) {
            break;
        }

        // accept it; a failure stops the server
        int error = 0;
        sockets::Socket accepted = sockets::acceptConnection(state->socket, &error);
        if (accepted == sockets::invalidSocket) {
            break;
        }

        // post MESSAGE_ACCEPTED with the socket in the value: the JS thread
        // wraps it in a Connection
        state->mailbox->post(state->source, MESSAGE_ACCEPTED, nullptr, 0, (int64_t)accepted);
    }

    // tell the JS thread, then let the state go (the last owner closes the
    // listening socket)
    state->mailbox->post(state->source, MESSAGE_CLOSED, nullptr, 0, 0);
    releaseSocketState(state);
}

/**
 * JS thread: starts 'run' on a new thread with its share of 'state'. If the
 * thread cannot start, both shares are let go (which closes the socket) and
 * false is returned.
 */
bool startSocketThread(platform::ThreadFunction *run, SocketState *state, const char *name) {
    // the thread owns its share from now on
    if (platform::startThread(run, state, platform::defaultThreadOptions(name))) {
        return true;
    }

    // never started: let go the thread's share, then the caller's (the
    // second one closes the socket and frees the state)
    releaseSocketState(state);
    releaseSocketState(state);
    return false;
}

// Bytes to send, borrowed from a JS string (UTF-8) or byte view: see
// outgoingData().
struct OutgoingData {
    const uint8_t *data;   // borrowed: points into text or buffer
    size_t size;           // bytes
    const char *text;      // to free with JS_FreeCString
    JSValue buffer;        // to free with JS_FreeValue
};

/**
 * Gets the bytes of a string (as UTF-8) or of a byte view (Uint8Array...)
 * without copying the view. Returns false if 'value' is neither. Release
 * with freeOutgoingData() in every case.
 */
bool outgoingData(JSContext *ctx, const js::Value *value, OutgoingData *out) {
    // a string: its UTF-8 C string
    *out = OutgoingData{ nullptr, 0, nullptr, JS_UNDEFINED };
    if (JS_IsString(value->get())) {
        out->text = JS_ToCStringLen(ctx, &out->size, value->get());
        out->data = (const uint8_t *)out->text;
        return out->text != nullptr;
    }

    // otherwise the bytes of a view, its buffer referenced meanwhile
    uint8_t *bytes = nullptr;
    bool ok = js::detail::bytesOf(ctx, value->get(), &bytes, &out->size, &out->buffer);
    out->data = bytes;
    return ok;
}

/**
 * Releases what outgoingData() took (the C string or the buffer reference).
 */
void freeOutgoingData(JSContext *ctx, OutgoingData *out) {
    JS_FreeCString(ctx, out->text);
    JS_FreeValue(ctx, out->buffer);
}

/**
 * A TCP connection (JS class Connection), made by connect() or by a Server.
 * Life cycle: start() registers the message source, starts the reader
 * thread and keeps the object alive; close() (or the peer) ends the reader
 * thread, whose MESSAGE_CLOSED removes the source, drops the keep-alive and
 * calls onClose. The socket lives in a SocketState shared with the reader
 * thread: the last of the two to let it go closes it.
 */
class Connection {
    JS_CLASS(Connection);

public:
    /**
     * Finalizer (no JS here): asks the reader thread to stop if it still
     * runs, and removes the source; the messages still queued for it are
     * then dropped.
     */
    ~Connection() {
        closeSocket();
        if (_loop != nullptr) {
            _loop->removeSource(_source);
        }
    }

    /**
     * C++ side (JS thread): takes an open socket and starts reading. If the
     * reader thread cannot start, the socket is closed, nothing stays
     * registered and false is returned (no error recorded: the caller
     * reports it).
     */
    bool start(sockets::Socket socket) {
        // register the message source, read the peer's address, share the
        // socket with the reader thread
        _loop = rt::Loop::current();
        _source = _loop->addSource(&handle, this);
        sockets::peerName(socket, _remoteAddress, sizeof(_remoteAddress), &_remotePort);
        SocketState *state = newSocketState(socket, _source);

        // start the reader thread; if it cannot start, the socket is already
        // closed: unregister the source
        if (!startSocketThread(&readLoop, state, "net-read")) {
            _loop->removeSource(_source);
            return false;
        }

        // open: the object stays alive until the close message
        _state = state;
        _open = true;
        js::keepAlive(this, true);
        return true;
    }

    /**
     * JS: callback(bytes) for each block received (a new Uint8Array).
     */
    JS_EXPORT(onData) void onData(js::Function callback) {
        _dataCallback = callback;
    }

    /**
     * JS: callback() once the connection is closed (by us or by the peer).
     */
    JS_EXPORT(onClose) void onClose(js::Function callback) {
        _closeCallback = callback;
    }

    /**
     * JS: sends a string (UTF-8) or bytes; the JS thread waits until the system
     * has taken all of them. Throws if closed, on a wrong type or if the send fails.
     */
    JS_EXPORT(write) void write(js::Value data) {
        // the connection must be open, and the socket still shared by this
        // side (close() lets it go at once)
        if (!_open || _state == nullptr) {
            return (void)js::fail("net: connection closed");
        }

        // borrow the bytes of the string or of the view
        JSContext *ctx = js::context();
        OutgoingData outgoing;
        if (!outgoingData(ctx, &data, &outgoing)) {
            freeOutgoingData(ctx, &outgoing);
            return (void)js::fail<js::TypeError>("net: write: data must be a string or bytes");
        }

        // send them all, then give them back
        int error = 0;
        bool sent = sockets::sendAll(_state->socket, outgoing.data, outgoing.size, &error);
        freeOutgoingData(ctx, &outgoing);
        if (!sent) {
            return (void)js::failSystem(error, "net: write");
        }
    }

    /**
     * JS: closes the socket now. The close callback is called later, when
     * the reader thread has stopped.
     */
    JS_EXPORT(close) void close() {
        closeSocket();
    }

    /**
     * JS: false once the close message has been handled.
     */
    JS_EXPORT(isOpen) bool isOpen() const {
        return _open;
    }

    /**
     * JS: IPv4 address of the peer ("" if unknown).
     */
    JS_EXPORT(remoteAddress) const char *remoteAddress() const {
        return _remoteAddress;
    }

    /**
     * JS: port of the peer (0 if unknown).
     */
    JS_EXPORT(remotePort) int remotePort() const {
        return _remotePort;
    }

private:
    /**
     * Asks the reader thread to stop and lets this side's share of the socket
     * go, once: the reader thread then posts MESSAGE_CLOSED, and the last
     * owner closes the socket.
     */
    void closeSocket() {
        if (_state != nullptr) {
            requestClose(_state);
            _state = nullptr;
        }
    }

    /**
     * Messages of the reader thread, on the JS thread: data, then the close.
     */
    static void handle(void *opaque, const rt::Message *message) {
        // MESSAGE_DATA: onData with a copy of the block (the loop frees the
        // block after this call). Called through a local copy of the
        // callback: the callback may replace itself (connection.onData(...)),
        // which would free the function while it runs
        Connection *connection = (Connection *)opaque;
        if (message->kind == MESSAGE_DATA) {
            js::Function callback = connection->_dataCallback;
            callback(js::Buffer::copy(message->data, message->size));
            return;
        }

        // MESSAGE_CLOSED, by us or by the peer (the reader thread has
        // stopped): mark the connection closed, let this side's share of the
        // socket go if still held (the peer closed), remove the source
        connection->_open = false;
        connection->closeSocket();
        connection->_loop->removeSource(connection->_source);

        // drop the keep-alive (the object may be collected after this), then
        // call onClose through a local copy of the callback, which stays valid
        // even if the object goes away
        js::Function callback = connection->_closeCallback;
        js::keepAlive(connection, false);
        callback();
    }

    SocketState *_state = nullptr;                      // this side's share of the socket; nullptr once let go
    rt::Loop *_loop = nullptr;                          // loop of the JS thread; nullptr before start()
    rt::SourceId _source = { -1, 0 };                   // message source of the reader thread
    bool _open = false;                                 // true from start() until the close message
    char _remoteAddress[64] = "";                       // peer IPv4 address; "" if unknown
    int _remotePort = 0;                                // peer port; 0 if unknown
    js::Function _dataCallback;                         // onData callback; empty: none
    js::Function _closeCallback;                        // onClose callback; empty: none
};

/**
 * A listening TCP server (JS class Server), made by listen(). Life cycle:
 * start() opens the listening socket and the accept thread and keeps the
 * object alive; close() ends the accept thread, whose MESSAGE_CLOSED
 * removes the source and drops the keep-alive.
 */
class Server {
    JS_CLASS(Server);

public:
    /**
     * Finalizer (no JS here): asks the accept thread to stop if it still
     * runs, and removes the source; the messages still queued for it are
     * then dropped.
     */
    ~Server() {
        closeSocket();
        if (_loop != nullptr) {
            _loop->removeSource(_source);
        }
    }

    /**
     * C++ side (JS thread), called by netListen(): starts listening. Returns
     * false with a pending error.
     */
    bool start(const char *host, int port, js::Function onConnection) {
        // open the listening socket (fails with a system error)
        int error = 0;
        sockets::Socket socket = sockets::listenOn(host, port, &_port, &error);
        if (socket == sockets::invalidSocket) {
            js::failSystem(error, "net: listen");
            return false;
        }

        // register the message source, share the socket with the accept thread
        _onConnection = onConnection;
        _loop = rt::Loop::current();
        _source = _loop->addSource(&handle, this);
        SocketState *state = newSocketState(socket, _source);

        // start the accept thread; if it cannot start, the socket is already
        // closed: unregister the source and fail
        if (!startSocketThread(&acceptLoop, state, "net-accept")) {
            _loop->removeSource(_source);
            js::fail("net: cannot start a thread");
            return false;
        }

        // the object stays alive until the close message
        _state = state;
        js::keepAlive(this, true);
        return true;
    }

    /**
     * JS: the port listened on (the one chosen by the system when 0 was
     * asked).
     */
    JS_EXPORT(port) int port() const {
        return _port;
    }

    /**
     * JS: stops listening; the accept thread then ends and the keep-alive
     * is dropped. The open connections stay open.
     */
    JS_EXPORT(close) void close() {
        closeSocket();
    }

private:
    /**
     * Asks the accept thread to stop and lets this side's share of the
     * listening socket go, once: the accept thread then posts MESSAGE_CLOSED,
     * and the last owner closes the socket.
     */
    void closeSocket() {
        if (_state != nullptr) {
            requestClose(_state);
            _state = nullptr;
        }
    }

    /**
     * Messages of the accept thread, on the JS thread: accepted sockets, then
     * the close.
     */
    static void handle(void *opaque, const rt::Message *message) {
        // MESSAGE_CLOSED (the accept thread has stopped): let this side's
        // share of the socket go if still held (accept failed), remove the
        // source and drop the keep-alive
        Server *server = (Server *)opaque;
        if (message->kind == MESSAGE_CLOSED) {
            server->closeSocket();
            server->_loop->removeSource(server->_source);
            js::keepAlive(server, false);
            return;
        }

        // MESSAGE_ACCEPTED: wrap the socket in a new Connection (close the
        // socket if the object cannot be made; its error is reported)
        sockets::Socket accepted = (sockets::Socket)message->value;
        js::Created<Connection> connection = js::create<Connection>();
        if (connection.object == nullptr) {
            sockets::closeSocket(accepted);
            return;
        }

        // start it (it closes the socket if its reader thread cannot start),
        // then hand it to onConnection
        if (!connection.object->start(accepted)) {
            return (void)js::fail("net: cannot start a thread");
        }

        server->_onConnection(connection.value);
    }

    SocketState *_state = nullptr;                      // this side's share of the listening socket; nullptr once let go
    int _port = 0;                                      // port listened on
    rt::Loop *_loop = nullptr;                          // loop of the JS thread; nullptr before start()
    rt::SourceId _source = { -1, 0 };                   // message source of the accept thread
    js::Function _onConnection;                         // called with each new Connection
};

struct ListenOptions {
    js::Optional<js::Str> host;   // default: 127.0.0.1 (not reachable from the network)
    int port = 0;                 // 0: any free port, see server.port()
};
JS_STRUCT(ListenOptions, host, port);

/**
 * True if the run may use host:port (rt::Permissions::mayUseNet); else
 * throws EACCES saying how to grant it, and returns false.
 */
bool netAllowed(const char *host, int port) {
    // the tool's own commands have no permissions: everything allowed
    const rt::Permissions *permissions = rt::permissions();
    if (permissions == nullptr || permissions->mayUseNet(host, port)) {
        return true;
    }

    // refused
    char what[300];
    const char *hint = permissions->hint();
    snprintf(what, sizeof(what), "net access to \"%s:%d\" is not allowed%s%s", host, port, hint[0] != 0 ? ": " : "",
             hint);
    js::failSystem(EACCES, what);
    return false;
}

/**
 * JS listen(options, onConnection): a new Server listening on options.host
 * (default 127.0.0.1) and options.port. Throws if it cannot listen, or if
 * the run may not use that address.
 */
js::Value netListen(ListenOptions options, js::Function onConnection) {
    // the address must be granted
    const char *host = options.host.has() ? options.host.value().data() : "127.0.0.1";
    if (!netAllowed(host, options.port)) {
        return js::Value();
    }

    // make the JS object
    js::Created<Server> server = js::create<Server>();
    if (server.object == nullptr) {
        return js::Value();
    }

    // start listening (the error is pending if it cannot)
    if (!server.object->start(host, options.port, onConnection)) {
        return js::Value();
    }

    return server.value;
}

struct ConnectOptions {
    js::Optional<js::Str> host;   // default: 127.0.0.1
    int port = 0;                 // port to connect to
};
JS_STRUCT(ConnectOptions, host, port);

// A connection being established by a thread.
struct PendingConnect {
    rt::Loop *loop;                     // loop of the JS thread, held until the answer
    rt::SourceId source;                // one-shot message source (connectDone)
    js::Resolver<js::Value> resolver;   // settles the promise of connect()
};

// What the connect thread needs (malloc'd, owned by the thread): a copy of
// the host name, since the JS string does not outlive the call.
struct ConnectRequest {
    char host[256];         // host name or address, NUL-terminated
    int port;               // port to connect to
    rt::Mailbox *mailbox;   // retained for the thread
    rt::SourceId source;    // message source of the PendingConnect
};

/**
 * Connect thread: connects (blocking) and posts the result to connectDone().
 * Owns 'argument', a ConnectRequest.
 */
void connectThread(void *argument) {
    ConnectRequest *request = (ConnectRequest *)argument;

    // connect; post MESSAGE_CONNECTED with the socket in the value, or
    // MESSAGE_FAILED with the error
    int error = 0;
    sockets::Socket socket = sockets::connectTo(request->host, request->port, &error);
    if (socket == sockets::invalidSocket) {
        request->mailbox->post(request->source, MESSAGE_FAILED, nullptr, 0, error);
    } else {
        request->mailbox->post(request->source, MESSAGE_CONNECTED, nullptr, 0, (int64_t)socket);
    }

    // the request is not needed anymore
    request->mailbox->release();
    memory::release(request);
}

/**
 * JS thread, the connection is established: resolves the promise of
 * connect() with a new Connection started on 'socket', or rejects it (the
 * socket closed) if the object or its reader thread cannot be made.
 */
void settleConnected(PendingConnect *pending, sockets::Socket socket) {
    // create the object inside a call scope of its own: its error must not
    // fail the current call (the loop's dispatch), which would then refuse to
    // run the rejection below
    JSContext *ctx = pending->loop->context();
    js::Created<Connection> connection = { nullptr, js::Value() };
    bool creationFailed = false;
    {
        js::detail::CallScope scope(ctx);
        connection = js::create<Connection>(ctx);
        creationFailed = scope.failed;
    }

    // not created (out of memory): drop its exception, close the socket and
    // reject the promise instead
    if (creationFailed) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        sockets::closeSocket(socket);
        pending->resolver.reject("net: connect: cannot create the connection");
        return;
    }

    // start it; if its reader thread cannot start (the socket is then
    // closed), reject the promise
    if (!connection.object->start(socket)) {
        pending->resolver.reject("net: cannot start a thread");
        return;
    }

    pending->resolver.resolve(connection.value);
}

/**
 * The answer of the connect thread, on the JS thread: settles the promise of
 * connect(). Owns 'opaque', a PendingConnect.
 */
void connectDone(void *opaque, const rt::Message *message) {
    // remove the one-shot source and the loop hold
    PendingConnect *pending = (PendingConnect *)opaque;
    pending->loop->removeSource(pending->source);
    pending->loop->unhold();

    // reject the promise with the system error, or resolve it with a new
    // Connection started on the socket
    if (message->kind == MESSAGE_FAILED) {
        pending->resolver.rejectSystem((int)message->value);
    } else {
        settleConnected(pending, (sockets::Socket)message->value);
    }

    delete pending;
}

/**
 * JS connect(options): a promise of a Connection, settled by connectDone().
 * Throws if the connect thread cannot start.
 */
js::Promise<js::Value> netConnect(ConnectOptions options) {
    // the address must be granted
    const char *host = options.host.has() ? options.host.value().data() : "127.0.0.1";
    if (!netAllowed(host, options.port)) {
        return js::Promise<js::Value>();
    }

    // the promise, a one-shot message source (connectDone) and a loop hold:
    // the loop must wait for the answer although no object is kept alive yet
    js::Promise<js::Value> promise = js::Promise<js::Value>::create();
    rt::Loop *loop = rt::Loop::current();
    PendingConnect *pending = new PendingConnect{ loop, { -1, 0 }, promise.resolver() };
    pending->source = loop->addSource(&connectDone, pending);
    loop->hold();   // the loop waits for the answer

    // the request of the thread: a copy of the host name, the port and the
    // mailbox (retained)
    ConnectRequest *request = (ConnectRequest *)memory::allocate(sizeof(ConnectRequest));
    snprintf(request->host, sizeof(request->host), "%s", host);
    request->port = options.port;
    request->mailbox = loop->mailbox();
    request->mailbox->retain();
    request->source = pending->source;

    // start the connect thread; if it cannot start, undo everything and throw
    if (!platform::startThread(&connectThread, request, platform::defaultThreadOptions("net-connect"))) {
        request->mailbox->release();
        memory::release(request);
        loop->removeSource(pending->source);
        loop->unhold();
        delete pending;
        return js::fail("net: cannot start a thread");
    }

    return promise;
}

} // namespace

/**
 * Module "sys/net": connect(), listen() and the classes Connection and Server.
 */
JS_MODULE(net, module) {
    JS_FUNCTION_AS(module, netConnect, "connect");   // C++ names: connect/listen are socket functions
    JS_FUNCTION_AS(module, netListen, "listen");
    JS_ADD_CLASS(module, Connection);
    JS_ADD_CLASS(module, Server);
}
