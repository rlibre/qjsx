/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "device/rmt": pulse trains of exact durations on a pin, sent by the
// hardware (hal_rmt.h), on the boards whose port implements it (registered by
// the port, see device_modules.h).
//
//   import { Rmt } from "device/rmt";
//   const rmt = new Rmt(48, { resolution: 10_000_000 });     // ticks per second: 1 tick = 0.1 us
//   await rmt.send([4, 1, 8, 0]);                            // [ticks, level, ticks, level, ...]
//   await rmt.sendBytes(bytes, { bit0: [4, 9], bit1: [8, 5], reset: 3000 });   // bits as [high, low] ticks
//   rmt.close();
//
// One send at a time: a second one while a promise is pending is refused.
// A pending send keeps the program running (js::keepAlive). The end of a send
// comes from the event task of the hal through a table of targets by channel
// (mailbox + source), never through a pointer to the object.
#include <stdio.h>

#include "jsbind.h"
#include "hal_rmt.h"
#include "loop.h"
#include "memory.h"
#include "platform.h"

namespace {

// Options of 'new Rmt(pin, options)'.
struct RmtOptions {
    js::Range<int, 1000, 80000000> resolution = 1000000;   // ticks per second
};
JS_STRUCT(RmtOptions, resolution);

// Options of 'rmt.sendBytes(bytes, options)'.
struct BytesOptions {
    js::Value bit0;                                    // [high, low] ticks of a 0 bit
    js::Value bit1;                                    // [high, low] ticks of a 1 bit
    js::Range<int, 0, 100000000> reset = 0;            // ticks low after the bytes; 0: none
};
JS_STRUCT(BytesOptions, bit0, bit1, reset);

const int MAX_CHANNELS = 16;      // channel numbers this module can hold (the hal may have fewer)
const int MESSAGE_DONE = 1;       // the send of the channel is over
const int MAX_TICKS = 32767;      // largest tick count of a bit (15 bits)

// Where the end of a send of a channel goes (JS thread writes, event task of
// the hal reads, under targetLock).
struct Target {
    rt::Mailbox *mailbox;  // mailbox of the loop of the run, retained; nullptr: no Rmt
    rt::SourceId source;   // the message source of the Rmt object
};

Target targets[MAX_CHANNELS];     // by channel; targetLock
platform::Mutex targetLock;       // targets

/**
 * Done handler given to the hal (its event task): posts the end of the send
 * to the loop of the Rmt of the channel, if any.
 */
void onSendDone(int channel) {
    targetLock.lock();
    Target target = targets[channel];
    if (target.mailbox != nullptr) {
        target.mailbox->post(target.source, MESSAGE_DONE, nullptr, 0, 0);
    }
    targetLock.unlock();
}

/**
 * Reads 'value' as a JS integer in [low, high] into *out. False (no JS
 * exception) if it is not one.
 */
bool integerIn(JSContext *ctx, JSValueConst value, int64_t low, int64_t high, int64_t *out) {
    if (!JS_IsNumber(value)) {
        return false;
    }
    if (JS_ToInt64(ctx, out, value) < 0) {
        return false;
    }
    return *out >= low && *out <= high;
}

/**
 * Reads a bit timing [high, low] (ticks) into *timing. False if it is not an
 * array of two tick counts.
 */
bool readBitTiming(JSContext *ctx, JSValueConst value, hal::rmt::BitTiming *timing) {
    if (!JS_IsArray(ctx, value)) {
        return false;
    }

    // the two items, each a tick count
    JSValue highValue = JS_GetPropertyUint32(ctx, value, 0);
    JSValue lowValue = JS_GetPropertyUint32(ctx, value, 1);
    int64_t high = 0;
    int64_t low = 0;
    bool ok = integerIn(ctx, highValue, 1, MAX_TICKS, &high) && integerIn(ctx, lowValue, 1, MAX_TICKS, &low);
    JS_FreeValue(ctx, highValue);
    JS_FreeValue(ctx, lowValue);

    timing->high = (uint16_t)high;
    timing->low = (uint16_t)low;
    return ok;
}

/**
 * A pulse train (JS class Rmt).
 */
class Rmt {
    JS_CLASS(Rmt);

public:
    /**
     * Finalizer (no JS here): closes the channel.
     */
    ~Rmt() {
        closeChannel();
    }

    /**
     * JS constructor: new Rmt(pin, { resolution }) opens a channel on the
     * pin. Throws if no channel is free or the pin is refused.
     */
    void init(js::Range<int, 0, 63> pin, js::Optional<RmtOptions> options) {
        // the channel
        RmtOptions settings = options.has() ? options.value() : RmtOptions();
        int channel = hal::rmt::open((int)pin, (uint32_t)(int)settings.resolution);
        if (channel < 0 || channel >= MAX_CHANNELS) {
            hal::rmt::close(channel);
            return (void)js::fail("rmt: cannot open a channel on pin %d", (int)pin);
        }

        // the end of its sends goes to this object, through the loop
        _channel = channel;
        _loop = rt::Loop::current();
        _source = _loop->addSource(&onMessage, this);
        hal::rmt::setDoneHandler(&onSendDone);

        targetLock.lock();
        targets[channel] = Target{ _loop->mailbox(), _source };
        targets[channel].mailbox->retain();
        targetLock.unlock();
    }

    /**
     * JS: sends pulses [ticks, level, ticks, level, ...]; a promise settled
     * when they have been sent.
     */
    JS_EXPORT(send) js::Promise<js::Value> send(js::Value pulses) {
        JSContext *ctx = js::context();
        if (!startSend()) {
            return js::Promise<js::Value>();
        }

        // the pulses, read from the array (an even number of integers)
        int64_t length = 0;
        bool isArray = JS_IsArray(ctx, pulses.get());
        if (isArray) {
            JSValue lengthValue = JS_GetPropertyStr(ctx, pulses.get(), "length");
            isArray = JS_ToInt64(ctx, &length, lengthValue) == 0;
            JS_FreeValue(ctx, lengthValue);
        }
        if (!isArray || length == 0 || length % 2 != 0) {
            _sending = false;
            return js::fail<js::TypeError>("rmt: send: pulses must be [ticks, level, ...], an even number of integers");
        }

        size_t count = (size_t)length / 2;
        hal::rmt::Pulse *list = (hal::rmt::Pulse *)memory::allocate(sizeof(hal::rmt::Pulse) * count);
        bool valid = true;

        for (size_t i = 0; i < count && valid; i++) {
            JSValue ticksValue = JS_GetPropertyUint32(ctx, pulses.get(), (uint32_t)(2 * i));
            JSValue levelValue = JS_GetPropertyUint32(ctx, pulses.get(), (uint32_t)(2 * i + 1));
            int64_t ticks = 0;
            int64_t level = 0;
            valid = integerIn(ctx, ticksValue, 1, 0xffffffffLL, &ticks) && integerIn(ctx, levelValue, 0, 1, &level);
            JS_FreeValue(ctx, ticksValue);
            JS_FreeValue(ctx, levelValue);
            list[i] = hal::rmt::Pulse{ (uint32_t)ticks, (int)level };
        }

        // send them (the hal copies them)
        bool sent = valid && hal::rmt::sendPulses(_channel, list, count);
        memory::release(list);
        if (!valid) {
            _sending = false;
            return js::fail<js::TypeError>("rmt: send: each pulse is a tick count (>= 1) and a level (0 or 1)");
        }
        return finishStart(sent);
    }

    /**
     * JS: sends bytes, most significant bit first, each bit as 'bit0' or
     * 'bit1' ([high, low] ticks), then 'reset' ticks low; a promise settled
     * when they have been sent.
     */
    JS_EXPORT(sendBytes) js::Promise<js::Value> sendBytes(js::Bytes bytes, BytesOptions options) {
        JSContext *ctx = js::context();
        if (!startSend()) {
            return js::Promise<js::Value>();
        }

        // the timing of the bits
        hal::rmt::BitTiming bit0 = {};
        hal::rmt::BitTiming bit1 = {};
        bool timed = readBitTiming(ctx, options.bit0.get(), &bit0) && readBitTiming(ctx, options.bit1.get(), &bit1);
        if (!timed || bytes.size() == 0) {
            _sending = false;
            return js::fail<js::TypeError>("rmt: sendBytes: bytes not empty, bit0 and bit1 as [high, low] ticks (1 to %d)", MAX_TICKS);
        }

        // send them (the hal copies them)
        bool sent = hal::rmt::sendBytes(_channel, bytes.data(), bytes.size(), bit0, bit1, (uint32_t)(int)options.reset);
        return finishStart(sent);
    }

    /**
     * JS: closes the channel (a pending send is rejected). The object can no
     * longer be used.
     */
    JS_EXPORT(close) void close() {
        if (_waiter.pending()) {
            _waiter.reject("rmt: closed while sending");
        }
        closeChannel();
        updateKeepAlive();
    }

private:
    /**
     * Checks that a send may start (open, none in progress); false with a
     * pending error otherwise. Marks the send started.
     */
    bool startSend() {
        if (_channel < 0) {
            js::fail("rmt: closed");
            return false;
        }
        if (_sending) {
            js::fail("rmt: a send is already in progress");
            return false;
        }
        _sending = true;
        return true;
    }

    /**
     * The promise of a send the hal accepted ('sent') or refused.
     */
    js::Promise<js::Value> finishStart(bool sent) {
        if (!sent) {
            _sending = false;
            return js::fail("rmt: the hardware refused the send");
        }

        js::Promise<js::Value> promise = js::Promise<js::Value>::create();
        _waiter = promise.resolver();
        updateKeepAlive();
        return promise;
    }

    /**
     * Closes the channel and forgets its target and source (no JS: also
     * called by the finalizer).
     */
    void closeChannel() {
        if (_channel < 0) {
            return;
        }

        hal::rmt::close(_channel);

        targetLock.lock();
        rt::Mailbox *mailbox = targets[_channel].mailbox;
        targets[_channel] = Target{ nullptr, { -1, 0 } };
        targetLock.unlock();
        if (mailbox != nullptr) {
            mailbox->release();
        }

        _loop->removeSource(_source);
        _loop = nullptr;
        _channel = -1;
        _sending = false;
    }

    /**
     * Keeps the object (and the program) alive while a send is pending.
     */
    void updateKeepAlive() {
        bool keep = _waiter.pending();
        if (keep != _kept) {
            js::keepAlive(this, keep);
            _kept = keep;
        }
    }

    /**
     * The send is over (JS thread): settles its promise.
     */
    static void onMessage(void *opaque, const rt::Message *) {
        Rmt *rmt = (Rmt *)opaque;
        rmt->_sending = false;
        if (rmt->_waiter.pending()) {
            rmt->_waiter.resolve(js::Value());
        }
        rmt->updateKeepAlive();
    }

    int                     _channel = -1;        // hal channel; -1: closed (or not initialized)
    bool                    _sending = false;     // a send is in progress
    js::Resolver<js::Value> _waiter;              // promise of the send in progress
    rt::Loop               *_loop = nullptr;      // loop of the run; nullptr: closed
    rt::SourceId            _source = { -1, 0 };  // message source of the ends of sends
    bool                    _kept = false;        // js::keepAlive(this, true) is in force
};

} // namespace

/**
 * The "device/rmt" module: the class Rmt.
 */
JS_MODULE(rmt, module) {
    JS_ADD_CLASS(module, Rmt);
}
