// Minimal portable layer over BSD sockets (Winsock on Windows; POSIX and
// lwIP elsewhere).
#pragma once

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

namespace sockets {

#if defined(_WIN32)
typedef SOCKET Socket;
const Socket invalidSocket = INVALID_SOCKET;
#else
typedef int Socket;
const Socket invalidSocket = -1;
#endif

/**
 * Initializes the socket library once (Winsock); harmless elsewhere.
 */
void initialize();

/**
 * errno-style code of the last socket error (ECONNREFUSED...).
 */
int lastError();

/**
 * Closes a socket. Never while another thread may still use it: on POSIX the
 * descriptor number is reused by the next open, so a late recv() would read
 * another file (see shutdownSocket).
 */
void closeSocket(Socket socket);

/**
 * Shuts both directions down, without closing: a thread blocked in recv() on
 * a connected socket returns (0 or an error), the peer sees the end of the
 * stream. Not every system wakes a thread blocked in accept() this way: a
 * thread that must stop waits with waitReadable() and checks a flag.
 */
void shutdownSocket(Socket socket);

/**
 * Waits at most timeoutMs for 'socket' to be readable (bytes, the end of the
 * stream, or a connection to accept). Returns 1 if readable, 0 after the
 * timeout, < 0 on error (sockets::lastError()).
 */
int waitReadable(Socket socket, int timeoutMs);

/**
 * Connects to host:port (blocking). Returns invalidSocket and sets *error.
 */
Socket connectTo(const char *host, int port, int *error);

/**
 * Listens on host:port (port 0: any free port). Returns invalidSocket and
 * sets *error. *boundPort receives the actual port.
 */
Socket listenOn(const char *host, int port, int *boundPort, int *error);

/**
 * Accepts a connection on a listening socket (blocking), set up like
 * connectTo()'s. Returns invalidSocket and sets *error.
 */
Socket acceptConnection(Socket listener, int *error);

/**
 * Sends everything. Returns false (and sets *error) on failure; a peer gone
 * is an error (EPIPE, ECONNRESET), never a SIGPIPE that kills the process.
 */
bool sendAll(Socket socket, const uint8_t *data, size_t size, int *error);

/**
 * Largest block received at once by receiveBlock().
 */
const size_t RECEIVE_BLOCK = 4096;

/**
 * Receives at most RECEIVE_BLOCK bytes into a new malloc'd block given to the
 * caller in *data (null unless the count is > 0). Returns the byte count, 0 at
 * the end of the stream, < 0 on error (sockets::lastError()).
 */
int receiveBlock(Socket socket, uint8_t **data);

/**
 * IPv4 address of the peer, written in 'text' ('size' bytes; 64 is
 * plenty), and its port in *port. Empty text and port 0 if the socket has
 * no peer.
 */
void peerName(Socket socket, char *text, size_t size, int *port);

} // namespace sockets
