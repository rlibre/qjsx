/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Serial ports: what the module device/serial (common/device/serial_module.cpp)
// needs from the machine. Unlike the other hal interfaces, this one is a
// DRIVER, a table of functions chosen when the process starts, because one
// executable may hold two kinds of ports: the qjsx tool has the serial ports
// of the PC, and the digital twin of a board, in the same executable, has the
// UARTs of that board. Three drivers:
//   - the PC (host/platform/serial_host.cpp): ports named by the system
//     ("COM3", "/dev/ttyUSB0"), no pins;
//   - a board (ports/esp32/main/hal_serial_esp32.cpp): the UARTs of the chip,
//     by number, on the pins the program gives;
//   - the twin (host/twin/sim/twin_uart.cpp): the UARTs of the board, each a
//     TCP port of the PC and a panel of the twin's page.
//
// Threads: open / write / close / list on the JS thread; read on the reader
// thread of the port (one per open port). read and write may run at the same
// time on one port; close is called once nobody reads any more (the module
// sees to it).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hal {
namespace serial {

enum class Parity {
    none,
    even,
    odd,
};

const int NO_PIN = -1;              // a signal of a board UART left unwired

// How a port is set up.
struct Settings {
    int    baudRate;                // bits per second
    int    dataBits;                // 5 to 8
    Parity parity;                  // parity bit
    int    stopBits;                // 1 or 2
    bool   flowControl;             // RTS / CTS hardware flow control
    int    tx;                      // board: pins of the signals; NO_PIN: not wired
    int    rx;                      //   (always NO_PIN for a named port)
    int    rts;
    int    cts;
};

// An open port, owned by the driver (opaque to the module).
struct Port;

// list() calls it once per port that can be opened: a named port gives its
// name (number -1), a numbered one its number (name nullptr).
typedef void ListHandler(void *context, const char *name, int number);

// A driver (see the top of the file).
struct Driver {
    // true: ports are named by the system (the PC) and have no pins; false:
    // ports are numbers (UARTs of a board) and the program gives the pins
    bool namedPorts;

    // Opens port 'name' (named driver) or 'number' (numbered driver) with
    // 'settings'. Returns nullptr and sets *error: ENOENT (no such port),
    // EBUSY (open already, or the link of the agent), EINVAL (settings
    // refused), EIO (anything else).
    Port *(*open)(const char *name, int number, const Settings *settings, int *error);

    // Reads what was received into 'buffer': returns as soon as bytes are
    // there, or after about 100 ms with 0; -1 on error (the port is then
    // closed by the module).
    int (*read)(Port *port, uint8_t *buffer, size_t size);

    // Sends all the bytes (blocks until the driver took them). False on error.
    bool (*write)(Port *port, const uint8_t *data, size_t size);

    // Closes the port and frees it (its UART can be opened again).
    void (*close)(Port *port);

    // The ports that can be opened now (see ListHandler).
    void (*list)(ListHandler *add, void *context);
};

/**
 * The driver of the UARTs of the board, on a device port (one per port:
 * ports/esp32/main/hal_serial_esp32.cpp).
 */
const Driver *boardDriver();

} // namespace serial
} // namespace hal
