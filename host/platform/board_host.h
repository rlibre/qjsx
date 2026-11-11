/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The desktop "board" of "qjsx agent" (board.h): packages in files, a TCP
// link (one host at a time), the console on stdout / stderr.
#pragma once

namespace board {

/**
 * Options of the desktop board, given by "qjsx agent".
 */
struct HostOptions {
    const char *host;      // address to listen on
    int         port;      // TCP port to listen on
    const char *storage;   // directory of <storage>/app.qjspkg and app.buildid
};

/**
 * Records the options and creates the storage directory. To call before anything
 * else of board.h. Returns false if the directory cannot be created (after printing why).
 */
bool configureHost(HostOptions options);

} // namespace board
