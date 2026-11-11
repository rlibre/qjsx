/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Pulse trains: what the module device/rmt needs from the hardware (the
// "hal"). A channel drives one pin with pulses of exact durations, counted in
// ticks of the resolution given to open(), without the CPU (the RMT
// peripheral of the ESP32; PIO on the RP2350, later). One implementation per
// port (ports/esp32/main/hal_rmt_esp32.cpp).
//
// Threads: every function is called on the JS thread; the done handler is
// called on the event task of the hal (never in an interrupt, never on the JS
// thread). A send copies what it sends: the caller's data can go at once.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hal {
namespace rmt {

// One pulse: the pin stays at 'level' for 'ticks'.
struct Pulse {
    uint32_t ticks;  // duration, in ticks of the channel (long ones are cut by the hal)
    int      level;  // 0 or 1
};

// How a bit of sendBytes() is sent: high for 'high' ticks, then low for 'low'.
struct BitTiming {
    uint16_t high;  // ticks at 1
    uint16_t low;   // ticks at 0
};

// Called on the event task of the hal when the send of 'channel' is over.
typedef void DoneHandler(int channel);

/**
 * Number of channel numbers: open() gives 0..channelCount() - 1.
 */
int  channelCount();

/**
 * Opens a sending channel on 'pin', counting 'resolutionHz' ticks per second;
 * the pin is low when idle. Returns its number, or -1 (no free channel, pin
 * or resolution refused).
 */
int  open(int pin, uint32_t resolutionHz);

/**
 * Stops the channel and frees it (a send in progress is dropped: no done).
 */
void close(int channel);

/**
 * The handler called when a send is over (one for every channel).
 */
void setDoneHandler(DoneHandler *handler);

/**
 * Sends the pulses (copied). One send at a time per channel: false if one is
 * in progress, or if the hardware refuses.
 */
bool sendPulses(int channel, const Pulse *pulses, size_t count);

/**
 * Sends the bytes (copied), most significant bit first, each bit as 'bit0'
 * or 'bit1', then keeps the pin low for 'resetTicks' (0: no reset). One send
 * at a time per channel: false if one is in progress, or if the hardware
 * refuses.
 */
bool sendBytes(int channel, const uint8_t *bytes, size_t size, BitTiming bit0, BitTiming bit1, uint32_t resetTicks);

} // namespace rmt
} // namespace hal
