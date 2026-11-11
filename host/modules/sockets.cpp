/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

#include "sockets.h"
#include "memory.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if !defined(_WIN32)
#include <poll.h>
#endif

namespace sockets {

#if defined(_WIN32)

/**
 * Starts Winsock 2.2 the first time.
 */
void initialize() {
    // the flag is not locked: two threads calling WSAStartup at once is
    // harmless (it counts its calls)
    static bool done = false;
    if (!done) {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        done = true;
    }
}

/**
 * Maps the last Winsock error to an errno code (EIO for the others), so
 * that JS sees the same codes as on POSIX.
 */
int lastError() {
    switch (WSAGetLastError()) {
        case WSAECONNREFUSED: {
            return ECONNREFUSED;
        }
        case WSAETIMEDOUT: {
            return ETIMEDOUT;
        }
        case WSAECONNRESET: {
            return ECONNRESET;
        }
        case WSAEADDRINUSE: {
            return EADDRINUSE;
        }
        case WSAEACCES: {
            return EACCES;
        }
        case WSAEINVAL: {
            return EINVAL;
        }
        default: {
            return EIO;
        }
    }
}

/**
 * Closes a socket (closesocket: a socket is not a file on Windows).
 */
void closeSocket(Socket socket) {
    closesocket(socket);
}

/**
 * Shuts both directions down; the socket stays open.
 */
void shutdownSocket(Socket socket) {
    shutdown(socket, SD_BOTH);
}

/**
 * Windows version: select() on the one socket (a Winsock fd_set is a list of
 * sockets, not a bit set: any socket value fits).
 */
int waitReadable(Socket socket, int timeoutMs) {
    // the set holds only this socket
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);

    // the timeout in seconds and microseconds
    timeval timeout;
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;

    // the first argument is ignored by Winsock
    int ready = select(0, &readable, nullptr, nullptr, &timeout);
    return ready > 0 ? 1 : ready;
}

#else

/**
 * Nothing to initialize on POSIX.
 */
void initialize() {}

/**
 * errno is already the right code on POSIX.
 */
int lastError() {
    return errno;
}

/**
 * Closes a socket (a file descriptor).
 */
void closeSocket(Socket socket) {
    close(socket);
}

/**
 * Shuts both directions down; the descriptor stays open.
 */
void shutdownSocket(Socket socket) {
    shutdown(socket, SHUT_RDWR);
}

/**
 * POSIX version: poll() on the one descriptor (select() cannot take a
 * descriptor above FD_SETSIZE).
 */
int waitReadable(Socket socket, int timeoutMs) {
    // readable, or the end of the stream / an error (reported in revents
    // without being asked: a recv() then returns it)
    pollfd watched;
    watched.fd = socket;
    watched.events = POLLIN;
    watched.revents = 0;

    // a signal interrupting the wait is not an error: the caller loops
    int ready = poll(&watched, 1, timeoutMs);
    if (ready < 0 && errno == EINTR) {
        return 0;
    }

    return ready > 0 ? 1 : ready;
}

#endif

namespace {

/**
 * Resolves host:port (IPv4) into *address. Returns false and sets *error
 * (ENOENT) if the name is unknown.
 */
bool resolve(const char *host, int port, sockaddr_in *address, int *error) {
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons((uint16_t)port);

    // an empty or null host means any local address
    if (host == nullptr || host[0] == 0) {
        address->sin_addr.s_addr = htonl(INADDR_ANY);
        return true;
    }

    // a numeric address is parsed
    if (inet_pton(AF_INET, host, &address->sin_addr) == 1) {
        return true;
    }

    // anything else is looked up: first IPv4 answer
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *found = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &found) != 0 || found == nullptr) {
        *error = ENOENT;   // like Node's ENOTFOUND
        return false;
    }

    address->sin_addr = ((sockaddr_in *)found->ai_addr)->sin_addr;
    freeaddrinfo(found);
    return true;
}

/**
 * Options of every connected socket (made by connectTo or acceptConnection).
 */
void setUpConnected(Socket socket) {
    // small messages (debugger protocol): send them right away (TCP_NODELAY)
    int noDelay = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (const char *)(&noDelay), sizeof(noDelay));

    // macOS has no MSG_NOSIGNAL for send() on older systems: the socket itself
    // is told not to raise SIGPIPE when the peer is gone (see sendAll)
#if defined(SO_NOSIGPIPE)
    int noSigpipe = 1;
    setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe));
#endif
}

} // namespace

/**
 * Connects to host:port (blocking). On failure the socket is closed, *error
 * is set and invalidSocket returned.
 */
Socket connectTo(const char *host, int port, int *error) {
    initialize();

    // resolve the address
    sockaddr_in address;
    if (!resolve(host, port, &address, error)) {
        return invalidSocket;
    }

    // create the socket
    Socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == invalidSocket) {
        *error = lastError();
        return invalidSocket;
    }

    // connect; blocks: the net module calls it from a thread of its own
    if (connect(socket, (sockaddr *)(&address), sizeof(address)) != 0) {
        *error = lastError();
        closeSocket(socket);
        return invalidSocket;
    }

    setUpConnected(socket);
    return socket;
}

/**
 * Accepts the next connection (blocking) and sets it up like connectTo().
 * On failure *error is set and invalidSocket returned.
 */
Socket acceptConnection(Socket listener, int *error) {
    Socket socket = accept(listener, nullptr, nullptr);
    if (socket == invalidSocket) {
        *error = lastError();
        return invalidSocket;
    }

    setUpConnected(socket);
    return socket;
}

/**
 * Listens on host:port. On failure the socket is closed, *error is set and
 * invalidSocket returned.
 */
Socket listenOn(const char *host, int port, int *boundPort, int *error) {
    initialize();

    // resolve the address
    sockaddr_in address;
    if (!resolve(host, port, &address, error)) {
        return invalidSocket;
    }

    // create the socket
    Socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == invalidSocket) {
        *error = lastError();
        return invalidSocket;
    }

    // SO_REUSEADDR on POSIX only: on Windows it would let another socket take
    // the same port
#if !defined(_WIN32)
    int reuse = 1;   // restart quickly on the same port
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    // bind and listen (backlog 8)
    bool ok = bind(socket, (sockaddr *)(&address), sizeof(address)) == 0
           && listen(socket, 8) == 0;
    if (!ok) {
        *error = lastError();
        closeSocket(socket);
        return invalidSocket;
    }

    // read back the bound port (the one chosen by the system when 'port' is 0)
    sockaddr_in bound;
    socklen_t length = sizeof(bound);
    getsockname(socket, (sockaddr *)(&bound), &length);
    *boundPort = ntohs(bound.sin_port);
    return socket;
}

/**
 * Receives into a new block given to the caller: reader threads post it to the
 * event loop as it is, without a buffer on their stack nor a copy.
 */
int receiveBlock(Socket socket, uint8_t **data) {
    // a block of the largest size received at once
    *data = nullptr;
    uint8_t *block = (uint8_t *)memory::allocate(RECEIVE_BLOCK);

    // receive into it; nothing (end or error): free it
    int received = recv(socket, (char *)block, (int)RECEIVE_BLOCK, 0);
    if (received <= 0) {
        memory::release(block);
        return received;
    }

    // shrink it to the received size
    *data = (uint8_t *)memory::reallocate(block, (size_t)received);
    return received;
}

/**
 * Sends everything, looping on partial sends.
 */
bool sendAll(Socket socket, const uint8_t *data, size_t size, int *error) {
    // on POSIX, writing to a peer that is gone raises SIGPIPE, which kills the
    // process: MSG_NOSIGNAL (Linux) turns it into an EPIPE error; where it
    // does not exist (older macOS), setUpConnected() set SO_NOSIGPIPE instead
#if defined(MSG_NOSIGNAL)
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif

    // by chunks of 64 KB at most: send() takes an int size
    while (size > 0) {
        int chunk = size > 65536 ? 65536 : (int)size;
        int sent = send(socket, (const char *)data, chunk, flags);
        if (sent <= 0) {
            *error = lastError();
            return false;
        }

        data += sent;
        size -= sent;
    }

    return true;
}

/**
 * Address (text) and port (host byte order) of the peer; empty and 0 if the
 * socket has no peer.
 */
void peerName(Socket socket, char *text, size_t size, int *port) {
    sockaddr_in address;
    socklen_t length = sizeof(address);
    text[0] = 0;
    *port = 0;

    // getpeername fails if the socket has no peer
    if (getpeername(socket, (sockaddr *)(&address), &length) == 0) {
        inet_ntop(AF_INET, &address.sin_addr, text, (socklen_t)size);
        *port = ntohs(address.sin_port);
    }
}

} // namespace sockets
