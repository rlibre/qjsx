/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * TCP client and server, IPv4 only.
 *
 * Not Node's `net`: events are set with `onData` / `onClose` (one callback
 * each), writes are synchronous, and `connect()` returns a promise.
 *
 * An open connection or a listening server keeps the program alive until it
 * is closed (by the program or, for a connection, by the peer).
 *
 * Permissions: a program of the PC may use only the addresses it was granted
 * (`qjsx run -N` / `--allow-net[=hosts]`, or `permissions.net` of the
 * `desktop` section of `qjsx.json`: `host` or `host:port`). A refusal is
 * thrown at once by `listen()` and `connect()`: an Error with `code`
 * `"EACCES"` whose message says how to grant it.
 *
 * System errors are `Error` objects with Node-like properties `code`
 * (`"ECONNREFUSED"`, `"ETIMEDOUT"`, `"ECONNRESET"`, `"EADDRINUSE"`,
 * `"EPIPE"`...; `"EUNKNOWN"` for a system error qjsx has no name for) and
 * `errno`.
 *
 * @example
 * import { connect, listen } from "sys/net";
 * const server = listen({ port: 0 }, connection => {
 *     connection.onData(bytes => connection.write(bytes));   // echo
 * });
 * const client = await connect({ port: server.port() });
 * client.onData(bytes => { console.log(new TextDecoder().decode(bytes)); client.close(); server.close(); });
 * client.write("hello");
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | ✓ | — | — | — |
 *
 * @module sys/net
 */
declare module "sys/net" {
    /**
     * Bytes accepted by `Connection.write`: any typed array (its own bytes) or
     * an ArrayBuffer. A DataView or an array of numbers is refused.
     */
    export type BinaryData = ArrayBuffer | Int8Array | Uint8Array | Uint8ClampedArray | Int16Array | Uint16Array | Int32Array | Uint32Array | Float16Array | Float32Array | Float64Array | BigInt64Array | BigUint64Array;

    /**
     * Options of {@link listen}. Unknown fields are a TypeError.
     */
    export interface ListenOptions {
        /**
         * Address or name to listen on (names resolved to their first IPv4
         * address). Default `"127.0.0.1"`: not reachable from the network.
         * `""` listens on every interface.
         */
        host?: string;

        /**
         * Port to listen on, an integer. Default 0: a free port chosen by the
         * system, see {@link Server.port}.
         */
        port?: number;
    }

    /**
     * Options of {@link connect}. Unknown fields are a TypeError.
     */
    export interface ConnectOptions {
        /**
         * Address or name of the peer (names resolved to their first IPv4
         * address). Default `"127.0.0.1"`.
         */
        host?: string;

        /**
         * Port of the peer, an integer (the runtime takes 0 when it is missing,
         * which never connects).
         */
        port: number;
    }

    /**
     * A TCP connection, given by {@link connect} or to the callback of
     * {@link listen}.
     *
     * Each connection has a reader thread: the received bytes come to
     * `onData` on the JS thread, and the connection keeps the program alive
     * until it is closed (by `close()` or by the peer), which calls `onClose`.
     */
    export class Connection {
        /**
         * A connection that is not connected: `isOpen()` is false and
         * `write()` throws. Programs get connections from {@link connect} and
         * {@link listen}; there is no reason to make one.
         */
        constructor();

        /**
         * Sets the function called with each block of bytes received (a new
         * Uint8Array; the blocks do not follow the sender's writes). One
         * callback at a time: a new call replaces it. Set it right away (in the
         * same turn as `connect()` resolves or the `listen()` callback runs):
         * bytes received while no callback is set are dropped.
         *
         * @throws TypeError if `callback` is not a function.
         */
        onData(callback: (bytes: Uint8Array) => void): void;

        /**
         * Sets the function called once the connection is closed, by this side
         * or by the peer (a network error included; the error is not given).
         * One callback at a time: a new call replaces it.
         *
         * @throws TypeError if `callback` is not a function.
         */
        onClose(callback: () => void): void;

        /**
         * Sends a string (as UTF-8) or bytes. Synchronous: the program waits
         * until the system has taken all of them.
         *
         * @throws Error (no `code`) once `close()` was called or the connection
         * closed; Error with `code` (`"ECONNRESET"`, `"EPIPE"`...) if the send
         * fails; TypeError if `data` is neither a string nor bytes.
         */
        write(data: string | BinaryData): void;

        /**
         * Closes the connection now. `onClose` is called later, once the reader
         * thread has stopped; the program stays alive until then. Calling it
         * again does nothing.
         */
        close(): void;

        /**
         * True from the connection until its close has been handled: it stays
         * true for a moment after `close()`, until `onClose` is called.
         */
        isOpen(): boolean;

        /**
         * IPv4 address of the peer (`""` if unknown).
         */
        remoteAddress(): string;

        /**
         * Port of the peer (0 if unknown).
         */
        remotePort(): number;
    }

    /**
     * A listening TCP server, given by {@link listen}. It keeps the program
     * alive until `close()`.
     */
    export class Server {
        /**
         * A server that does not listen (`port()` is 0). Programs get servers
         * from {@link listen}; there is no reason to make one.
         */
        constructor();

        /**
         * The port listened on: the one chosen by the system when 0 was asked.
         */
        port(): number;

        /**
         * Stops listening. The connections already accepted stay open. Calling
         * it again does nothing.
         */
        close(): void;
    }

    /**
     * Starts listening on `options.host` : `options.port` (backlog 8) and
     * calls `onConnection` with each accepted connection, already open.
     *
     * @throws Error with `code` (`"EACCES"` when the address is not granted or
     * the system refuses it, `"EADDRINUSE"`, `"ENOENT"` for an unknown host
     * name...); TypeError for bad options (`options` is required, an object).
     */
    export function listen(options: ListenOptions, onConnection: (connection: Connection) => void): Server;

    /**
     * Connects to `options.host` : `options.port` from a thread of its own;
     * the program stays alive until the promise settles.
     *
     * @returns a promise of the open connection. It rejects with an Error with
     * `code`: `"ECONNREFUSED"`, `"ETIMEDOUT"`, `"ENOENT"` for an unknown host
     * name (Node's `ENOTFOUND`)...
     * @throws Error with `code` `"EACCES"` (at once, not as a rejection) when
     * the address is not granted; TypeError for bad options.
     */
    export function connect(options: ConnectOptions): Promise<Connection>;
}
