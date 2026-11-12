/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// USB device: what the module device/usb needs from the hardware (the "hal").
// The chip is a USB device for a host (a PC, a tablet): it presents the
// descriptors given to start() and up to MAX_SERIALS serial functions (CDC
// ACM). One implementation per port (ports/esp32/main/hal_usb_esp32.cpp, with
// TinyUSB) and one in the digital twin (host/twin/sim/twin_usb.cpp, over TCP).
//
// The device outlives the programs: it stays started until stop(), so that a
// new program can take it again without the host seeing the device go away.
//
// Threads: start / stop / write / lineState on the JS thread; the handlers
// are called on the task of the hal (never in an interrupt, never on the JS
// thread).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hal {
namespace usb {

const int MAX_SERIALS = 2;         // serial functions of a device (the limit of esp_tinyusb)
const int MAX_TEXT = 31;           // longest descriptor text, in ASCII characters

// What the host sees of the device.
struct Descriptors {
    uint16_t vendorId;                       // 0: the default of the chip's vendor
    uint16_t productId;                      // 0: the default of the chip's vendor
    char     manufacturer[MAX_TEXT + 1];     // printable ASCII
    char     product[MAX_TEXT + 1];          // printable ASCII
    char     serialNumber[MAX_TEXT + 1];     // printable ASCII; "": none
};

// Bytes received on serial function 'serial': 'data' comes from
// memory::allocate() and the handler owns it.
typedef void DataHandler(int serial, uint8_t *data, size_t size);

// The host changed the line state of serial function 'serial' (DTR: a
// program opened the port on the host side).
typedef void LineStateHandler(int serial, bool dtr, bool rts);

/**
 * Sets the handlers of the received bytes and of the line state changes
 * (nullptr: none, the received bytes are dropped). Any time.
 */
void setHandlers(DataHandler *data, LineStateHandler *lineState);

/**
 * Starts the device with 'descriptors' and 'serials' serial functions (1 to
 * MAX_SERIALS); the host then sees it. False if the hardware refused (the
 * device stays stopped). Must not be started already.
 */
bool start(const Descriptors *descriptors, int serials);

/**
 * Stops the device: the host sees it go away. Nothing if it is stopped.
 */
void stop();

/**
 * Sends bytes on serial function 'serial' (copied before it returns). What
 * the host does not take within about 50 ms is dropped, all of it when no
 * host is there, as a UART with nobody at the other end: the result is
 * still true. DTR is not a condition (a host may read without raising it).
 * False only if the device is stopped.
 */
bool write(int serial, const uint8_t *data, size_t size);

/**
 * The line state of serial function 'serial' now (false, false when the
 * device is stopped).
 */
void lineState(int serial, bool *dtr, bool *rts);

} // namespace usb
} // namespace hal
