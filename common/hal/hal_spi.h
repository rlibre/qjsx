/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// SPI master: what the module device/spi needs from the hardware (the
// "hal"). A bus is a controller of the chip on its clock and data pins; a
// device is a chip select on a bus with its own frequency, mode, bit order and
// optional data / command (DC) pin. One implementation per port
// (ports/esp32/main/hal_spi_esp32.cpp) and one in the digital twin
// (host/twin/sim/twin_spi.cpp, where the parts of the project answer).
//
// A transaction is a list of segments run with the chip select held from the
// first segment to the last. Each segment writes, reads (sending the device's
// fill byte), exchanges (full duplex) or waits; the DC pin, when the device
// has one, is set to the segment's level just before its bytes go out. A
// segment may be of any size: the implementation cuts it in pieces of at most
// the bus's maxTransfer bytes, the chip select kept active.
//
// Threads: the module never runs two transactions on the same bus at once.
// run() blocks; start() returns at once and calls 'done' later; finish() is
// called by the module on its own thread once 'done' was received, before the
// next transaction of that bus.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hal {
namespace spi {

// How opening a bus, adding a device or a transaction ended.
enum class Status {
    ok,                 // done
    noSuchPort,         // the chip has no such controller
    busy,               // the controller is taken (open already, or kept for the flash)
    badPin,             // a pin the chip cannot use for this
    failed,             // any other error of the hardware or the driver
};

// The settings of a device (addDevice).
struct DeviceConfig {
    int      cs;                // chip select pin, -1 = none (the program drives it)
    int      dc;                // data / command pin, -1 = none
    uint32_t frequencyHz;       // clock asked for
    int      mode;              // 0..3: bit 1 = CPOL, bit 0 = CPHA
    bool     lsbFirst;          // bit order: least significant bit first
    bool     csActiveHigh;      // chip select active at level 1
    uint8_t  fill;              // byte sent while reading
};

// One segment of a transaction. Exactly one kind:
//   write:    'write' set, 'read' null, 'size' bytes sent;
//   read:     'write' null, 'read' set, 'size' bytes received, the fill byte sent;
//   transfer: both set, 'size' bytes sent and received at the same time;
//   delay:    both null, 'size' 0, waits 'delayUs' with the chip select active;
//   made:     both null, 'make' set, 'size' bytes sent, made piece by piece
//             just before each piece goes out (a display's pixels rendered a
//             few rows at a time: no memory for the whole of them).
struct Segment {
    const uint8_t *write;       // bytes to send, or null; owned by the caller
    uint8_t       *read;        // where received bytes go, or null; owned by the caller
    size_t         size;        // bytes of the segment
    int            dc;          // level of the DC pin during the segment (0 or 1)
    uint32_t       delayUs;     // delay segment only: microseconds
    void         (*make)(void *opaque, uint8_t *bytes, size_t offset, size_t size);
                                // made segment only: fills 'bytes' with the 'size' bytes at 'offset' of
                                // the segment; called on the thread that runs the transaction
    void          *makeOpaque;  // made segment only: given to 'make'; owned by the caller
    uint8_t       *piece;       // made segment only: room for TWO pieces (2 x pieceSize, DMA-capable
                                // memory): one is made while the other goes out; owned by the caller
    size_t         pieceSize;   // made segment only: bytes of a piece (the last one may be shorter),
                                // at most the bus's maxTransfer
};

// Called once when a started transaction is over, from a thread (never from an
// interrupt: the module posts a message to the loop, which takes a lock and
// allocates), never from the program's JS thread loop: a Sync call or close()
// waits for it with the JS thread blocked. The read bytes are in place when it
// is called.
typedef void (*DoneCallback)(void *opaque, Status status);

/**
 * Whether 'port' names an SPI controller the program may open (ESP32-S3: 2
 * and 3; the controllers of the flash are not).
 */
bool   portExists(int port);

/**
 * Takes controller 'port' on its pins (-1 = not wired; at least one of mosi /
 * miso), for transfers of up to 'maxTransfer' bytes in one go (DMA).
 */
Status openBus(int port, int sclk, int mosi, int miso, size_t maxTransfer);

/**
 * Frees the bus and its pins. Its devices must be removed first.
 */
void   closeBus(int port);

/**
 * Adds a device on the open bus 'port'. On success '*device' is its number
 * and '*actualHz' the clock the hardware really makes.
 */
Status addDevice(int port, const DeviceConfig *config, int *device, uint32_t *actualHz);

/**
 * Removes a device added by addDevice(); its bus must be idle.
 */
void   removeDevice(int device);

/**
 * Runs a transaction of 'count' segments on 'device' and waits for its end.
 */
Status run(int device, const Segment *segments, size_t count);

/**
 * Starts a transaction and returns at once; 'done' is called when it is over
 * (also when it could not start: then before start() returns). The segments
 * and their bytes stay valid until finish().
 */
void   start(int device, const Segment *segments, size_t count, DoneCallback done, void *opaque);

/**
 * Ends a started transaction once its 'done' was received: frees what start()
 * took (ESP32: the bus acquired for the device). On the module's thread.
 */
void   finish(int device);

} // namespace spi
} // namespace hal
