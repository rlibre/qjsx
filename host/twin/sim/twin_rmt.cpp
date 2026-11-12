/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// hal::rmt of the digital twin (see twin.h): a send takes the time the
// hardware would take (its ticks over the resolution), then the done handler
// is called on a thread of its own, as the event task of a board would. What
// is sent goes to the components through the bridge (twin.h): the bytes of
// sendBytes (a WS2812 decodes its colors), the pulses of sendPulses.
//
// Threads: open / close / send on the JS thread; the "twin rmt" thread waits
// for the end of the sends and calls the done handler; channelLock protects
// the channels.
#include "hal_rmt.h"
#include "twin.h"
#include "memory.h"
#include "platform.h"

namespace {

const int CHANNELS = 8;              // as the ESP32-S3: 4 TX + 4 RX, all usable here

// A channel of the twin (under channelLock).
struct Channel {
    bool     open;                   // open() gave it
    int      pin;                    // the pin it drives
    uint32_t resolutionHz;           // ticks per second
    bool     busy;                   // a send is in progress
    int64_t  endMs;                  // when the send in progress ends (monotonicMs)
};

Channel channels[CHANNELS];          // channelLock
platform::Signal channelLock;        // channels, doneHandler, threadStarted; wakes the thread
hal::rmt::DoneHandler *doneHandler = nullptr;   // channelLock
bool threadStarted = false;          // channelLock

/**
 * Milliseconds that 'ticks' last at 'resolutionHz' (rounded up, at least 1:
 * the done handler never comes before the send returns).
 */
int64_t durationMs(uint64_t ticks, uint32_t resolutionHz) {
    uint64_t milliseconds = (ticks * 1000 + resolutionHz - 1) / resolutionHz;
    return milliseconds > 0 ? (int64_t)milliseconds : 1;
}

/**
 * The "twin rmt" thread: waits for the earliest end of send, marks the
 * channel free and calls the done handler (outside the lock).
 */
void runChannels(void *) {
    channelLock.lock();
    for (;;) {
        // the earliest send in progress
        int64_t now = platform::monotonicMs();
        int next = -1;
        for (int i = 0; i < CHANNELS; i++) {
            bool earlier = next < 0 || channels[i].endMs < channels[next].endMs;
            if (channels[i].busy && earlier) {
                next = i;
            }
        }

        // nothing in progress: wait for a send
        if (next < 0) {
            channelLock.wait(-1);
            continue;
        }

        // not over yet: wait for its end (or a new send, which may end sooner)
        if (channels[next].endMs > now) {
            channelLock.wait(channels[next].endMs - now);
            continue;
        }

        // over: free the channel, tell the module
        channels[next].busy = false;
        hal::rmt::DoneHandler *handler = doneHandler;
        channelLock.unlock();
        if (handler != nullptr) {
            handler(next);
        }

        channelLock.lock();
    }
}

/**
 * Starts a send on 'channel' that lasts 'ticks'; false if the channel is
 * not open or busy. Starts the thread at the first send.
 */
bool startSend(int channel, uint64_t ticks) {
    if (channel < 0 || channel >= CHANNELS) {
        return false;
    }

    // the channel must be open and free
    channelLock.lock();
    Channel *target = &channels[channel];
    if (!target->open || target->busy) {
        channelLock.unlock();
        return false;
    }

    // the thread, once
    if (!threadStarted) {
        threadStarted = platform::startThread(&runChannels, nullptr, platform::defaultThreadOptions("twin rmt"));
    }

    // the send, and wake the thread: its end may be the earliest
    target->busy = true;
    target->endMs = platform::monotonicMs() + durationMs(ticks, target->resolutionHz);
    channelLock.notify();
    channelLock.unlock();
    return threadStarted;
}

} // namespace

namespace hal {
namespace rmt {

/**
 * Channels of the twin.
 */
int channelCount() {
    return CHANNELS;
}

/**
 * Takes a free channel for 'pin'.
 */
int open(int pin, uint32_t resolutionHz) {
    if (pin < 0 || pin >= twin::pinCount() || resolutionHz == 0) {
        return -1;
    }

    // the first free channel
    channelLock.lock();
    int found = -1;
    for (int i = 0; i < CHANNELS && found < 0; i++) {
        if (!channels[i].open) {
            found = i;
        }
    }

    if (found >= 0) {
        channels[found] = Channel{ true, pin, resolutionHz, false, 0 };
    }

    channelLock.unlock();

    // the page shows the pin driven by the RMT
    if (found >= 0) {
        twin::setRmt(pin, true);
    }

    return found;
}

/**
 * Frees the channel; a send in progress is dropped (no done).
 */
void close(int channel) {
    if (channel < 0 || channel >= CHANNELS) {
        return;
    }

    // free it
    channelLock.lock();
    bool wasOpen = channels[channel].open;
    int pin = channels[channel].pin;
    channels[channel].open = false;
    channels[channel].busy = false;
    channelLock.unlock();

    // the pin is not driven by the RMT any more
    if (wasOpen) {
        twin::setRmt(pin, false);
    }
}

/**
 * The handler of the ends of sends.
 */
void setDoneHandler(DoneHandler *handler) {
    channelLock.lock();
    doneHandler = handler;
    channelLock.unlock();
}

/**
 * Sends pulses: their duration, and the pulses to the components (bridge:
 * ticks u32 LE then level u8, per pulse).
 */
bool sendPulses(int channel, const Pulse *pulses, size_t count) {
    // their total duration
    uint64_t ticks = 0;
    for (size_t i = 0; i < count; i++) {
        ticks += pulses[i].ticks;
    }

    // the send
    if (!startSend(channel, ticks)) {
        return false;
    }

    // the pulses, packed, for the components watching that pin
    channelLock.lock();
    int pin = channels[channel].pin;
    channelLock.unlock();
    size_t size = count * 5;
    uint8_t *packed = (uint8_t *)memory::allocate(size > 0 ? size : 1);
    for (size_t i = 0; i < count; i++) {
        uint8_t *entry = packed + i * 5;
        for (int byte = 0; byte < 4; byte++) {
            entry[byte] = (uint8_t)(pulses[i].ticks >> (8 * byte));
        }
        entry[4] = (uint8_t)(pulses[i].level != 0 ? 1 : 0);
    }

    twin::bridgePost(twin::BridgeEvent::rmtPulses, pin, packed, size);
    memory::release(packed);
    return true;
}

/**
 * Sends bytes: their duration, and the bytes to the components (bridge).
 */
bool sendBytes(int channel, const uint8_t *bytes, size_t size, BitTiming bit0, BitTiming bit1, uint32_t resetTicks) {
    // the duration: every bit, then the reset
    uint64_t ticks = resetTicks;
    for (size_t i = 0; i < size; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            BitTiming timing = (bytes[i] >> bit) & 1 ? bit1 : bit0;
            ticks += (uint64_t)timing.high + timing.low;
        }
    }

    // the send
    if (!startSend(channel, ticks)) {
        return false;
    }

    // the bytes, for the components watching that pin (a WS2812 decodes them)
    channelLock.lock();
    int pin = channels[channel].pin;
    channelLock.unlock();
    twin::bridgePost(twin::BridgeEvent::rmtBytes, pin, bytes, size);
    return true;
}

} // namespace rmt
} // namespace hal
