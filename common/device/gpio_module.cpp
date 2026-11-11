/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "device/gpio": digital pins, on the boards whose port implements
// hal_gpio.h (registered by the port, see device_modules.h).
//
//   import { Gpio } from "device/gpio";
//   const led = new Gpio(1, { mode: "output" });       // "input" | "output" | "openDrain"
//   led.write(1);                                       // 1 / 0, true / false
//   led.toggle();
//   const button = new Gpio(0, { mode: "input", pull: "up" });   // "none" | "up" | "down"
//   button.read();                                      // 0 or 1
//   button.onChange(level => ..., { edge: "falling", debounce: 20 });   // "rising" | "falling" | "both", ms
//   await button.waitFor(0, { timeout: 5000 });          // rejects after the timeout
//   led.close();                                        // the pin goes back as at boot
//
// Pins are GPIO numbers; nothing is checked (the program is responsible).
// A pin with an onChange callback or a pending waitFor keeps the program
// running (js::keepAlive), until close(). An object collected without
// close() stops watching but leaves its pin as it is (an output keeps its
// level).
//
// Changes come from the event task of the hal: it posts them to the event
// loop of the JS thread through a table of targets by pin (mailbox + source),
// never through a pointer to the object.
#include <stdio.h>

#include "jsbind.h"
#include "hal_gpio.h"
#include "loop.h"
#include "platform.h"

namespace {

JS_ENUM(Mode, input, output, openDrain);
JS_ENUM(Pull, none, up, down);
JS_ENUM(Edge, rising, falling, both);

// Options of 'new Gpio(pin, options)'.
struct GpioOptions {
    Mode mode = Mode::input;  // JS: "input", "output" or "openDrain"
    Pull pull = Pull::none;   // JS: "none", "up" or "down"
};
JS_STRUCT(GpioOptions, mode, pull);

// Options of 'gpio.onChange(callback, options)'.
struct ChangeOptions {
    Edge edge = Edge::both;                   // which changes call the callback
    js::Range<int, 0, 60000> debounce = 0;    // ms the level must stay the same; 0: none
};
JS_STRUCT(ChangeOptions, edge, debounce);

// Options of 'gpio.waitFor(level, options)'.
struct WaitOptions {
    js::Optional<js::Range<int, 0, 86400000>> timeout;   // ms; absent: no limit
};
JS_STRUCT(WaitOptions, timeout);

const int MAX_PINS = 64;          // GPIO numbers this module can hold (the hal may have fewer)
const int MESSAGE_CHANGE = 1;     // a watched pin changed; value: its level

// Where the changes of a watched pin go (JS thread writes, event task of the
// hal reads, under targetLock).
struct Target {
    rt::Mailbox *mailbox;  // mailbox of the loop of the run, retained; nullptr: not watched
    rt::SourceId source;   // the message source of the Gpio object
};

Target targets[MAX_PINS];        // by pin; targetLock
platform::Mutex targetLock;      // targets

class Gpio;
Gpio *owners[MAX_PINS];          // the open Gpio of each pin (JS thread): the timeout of waitFor finds it

/**
 * Change handler given to the hal (its event task): posts the level to the
 * loop of the Gpio watching the pin, if any.
 */
void onPinChange(int pin, int level) {
    targetLock.lock();
    Target target = targets[pin];
    if (target.mailbox != nullptr) {
        target.mailbox->post(target.source, MESSAGE_CHANGE, nullptr, 0, level);
    }
    targetLock.unlock();
}

/**
 * A digital pin (JS class Gpio).
 */
class Gpio {
    JS_CLASS(Gpio);

public:
    /**
     * Finalizer (no JS here): stops watching; the pin keeps its state.
     */
    ~Gpio() {
        stopWatching();
        if (_pin >= 0 && owners[_pin] == this) {
            owners[_pin] = nullptr;
        }
    }

    /**
     * JS constructor: new Gpio(pin, { mode, pull }) configures the pin.
     * Throws if the chip refuses it.
     */
    void init(js::Range<int, 0, MAX_PINS - 1> pin, js::Optional<GpioOptions> options) {
        // the pin must exist on this chip
        int number = pin;
        if (number >= hal::gpio::pinCount()) {
            return (void)js::fail<js::RangeError>("gpio: no pin %d on this chip", number);
        }

        // configure it
        GpioOptions settings = options.has() ? options.value() : GpioOptions();
        bool configured = hal::gpio::configure(number, halMode(settings.mode), halPull(settings.pull));
        if (!configured) {
            return (void)js::fail("gpio: pin %d cannot be configured as asked", number);
        }

        _pin = number;
        owners[number] = this;
    }

    /**
     * JS: the level of the pin, 0 or 1.
     */
    JS_EXPORT(read) int read() {
        if (!isOpen()) {
            return js::fail("gpio: closed");
        }
        return hal::gpio::read(_pin);
    }

    /**
     * JS: sets the level of an output (1, 0, true, false: any value, as a
     * condition). The fast path of the module.
     */
    JS_EXPORT(write) void write(js::Value level) {
        if (!isOpen()) {
            return (void)js::fail("gpio: closed");
        }
        int high = JS_ToBool(js::context(), level.get()) > 0 ? 1 : 0;
        hal::gpio::write(_pin, high);
    }

    /**
     * JS: inverts the level of an output.
     */
    JS_EXPORT(toggle) void toggle() {
        if (!isOpen()) {
            return (void)js::fail("gpio: closed");
        }
        hal::gpio::toggle(_pin);
    }

    /**
     * JS: callback(level) at each change of the level that 'edge' selects,
     * once the level has stayed the same for 'debounce' ms. Replaces the
     * previous callback. Keeps the program running until close().
     */
    JS_EXPORT(onChange) void onChange(js::Function callback, js::Optional<ChangeOptions> options) {
        if (!isOpen()) {
            return (void)js::fail("gpio: closed");
        }

        // remember the callback and its filter
        ChangeOptions settings = options.has() ? options.value() : ChangeOptions();
        _changeCallback = callback;
        _edge = settings.edge;
        _debounceMs = settings.debounce;

        // watch the pin (again, with this debounce)
        if (!startWatching()) {
            return (void)js::fail("gpio: cannot watch pin %d", _pin);
        }
        updateKeepAlive();
    }

    /**
     * JS: a promise settled when the pin reads 'level' (at once if it does
     * already), rejected after 'timeout' ms or by close(). One at a time.
     */
    JS_EXPORT(waitFor) js::Promise<js::Value> waitFor(js::Range<int, 0, 1> level, js::Optional<WaitOptions> options) {
        if (!isOpen()) {
            return js::fail("gpio: closed");
        }
        if (_waiter.pending()) {
            return js::fail("gpio: a waitFor is already pending on pin %d", _pin);
        }

        // already there: settled at once
        js::Promise<js::Value> promise = js::Promise<js::Value>::create();
        int wanted = level;
        if (hal::gpio::read(_pin) == wanted) {
            js::Resolver<js::Value> resolver = promise.resolver();
            resolver.resolve(js::Value());
            return promise;
        }

        // else wait for the change, watching the pin
        _waiter = promise.resolver();
        _waitLevel = wanted;
        if (!startWatching()) {
            _waiter.reject("gpio: cannot watch pin %d", _pin);
            return promise;
        }

        // the timeout, as a timer of the loop
        bool hasTimeout = options.has() && options.value().timeout.has();
        if (hasTimeout) {
            int timeoutMs = options.value().timeout.value();
            _waitTimer = rt::Loop::current()->addTimer(timeoutMs, false, timeoutFunction(), js::Args{ js::context(), 0, nullptr });
        }

        updateKeepAlive();
        return promise;
    }

    /**
     * JS: stops watching, rejects a pending waitFor, puts the pin back as at
     * boot. The object can no longer be used.
     */
    JS_EXPORT(close) void close() {
        if (!isOpen()) {
            return;
        }

        // a pending wait fails
        if (_waiter.pending()) {
            clearWaitTimer();
            _waiter.reject("gpio: closed while waiting");
        }

        // no watch, the pin as at boot, the object no longer kept
        _changeCallback = js::Function();
        stopWatching();
        hal::gpio::reset(_pin);
        owners[_pin] = nullptr;
        _pin = -1;
        updateKeepAlive();
    }

    /**
     * The timer of waitFor ran out: rejects the wait (JS thread).
     */
    void waitTimedOut() {
        _waitTimer = 0;
        if (_waiter.pending()) {
            _waiter.reject("gpio: waitFor timed out");
        }
        stopWatchingIfUnused();
        updateKeepAlive();
    }

private:
    /**
     * True until close().
     */
    bool isOpen() const {
        return _pin >= 0;
    }

    /**
     * The hal mode of a JS mode.
     */
    static hal::gpio::Mode halMode(Mode mode) {
        switch (mode) {
            case Mode::output: {
                return hal::gpio::Mode::output;
            }
            case Mode::openDrain: {
                return hal::gpio::Mode::openDrain;
            }
            default: {
                return hal::gpio::Mode::input;
            }
        }
    }

    /**
     * The hal pull of a JS pull.
     */
    static hal::gpio::Pull halPull(Pull pull) {
        switch (pull) {
            case Pull::up: {
                return hal::gpio::Pull::up;
            }
            case Pull::down: {
                return hal::gpio::Pull::down;
            }
            default: {
                return hal::gpio::Pull::none;
            }
        }
    }

    /**
     * Watches the pin (JS thread): a message source in the loop, the target
     * of the pin, the hal watch with the current debounce (changed if
     * already watching). False if the hal cannot watch it.
     */
    bool startWatching() {
        // the source and the target, once
        if (_loop == nullptr) {
            _loop = rt::Loop::current();
            _source = _loop->addSource(&onMessage, this);

            targetLock.lock();
            targets[_pin] = Target{ _loop->mailbox(), _source };
            targets[_pin].mailbox->retain();
            targetLock.unlock();
        }

        // the hal watch: the debounce of onChange, none for a wait alone
        int debounceMs = _changeCallback.isUndefined() ? 0 : _debounceMs;
        return hal::gpio::watch(_pin, debounceMs, &onPinChange);
    }

    /**
     * Stops watching: hal watch, target, source (no JS: also called by the
     * finalizer). The changes already queued are dropped with the source.
     */
    void stopWatching() {
        if (_loop == nullptr) {
            return;
        }

        hal::gpio::unwatch(_pin);

        targetLock.lock();
        rt::Mailbox *mailbox = targets[_pin].mailbox;
        targets[_pin] = Target{ nullptr, { -1, 0 } };
        targetLock.unlock();
        mailbox->release();

        _loop->removeSource(_source);
        _loop = nullptr;
    }

    /**
     * Stops watching when neither a callback nor a wait needs it.
     */
    void stopWatchingIfUnused() {
        if (_changeCallback.isUndefined() && !_waiter.pending()) {
            stopWatching();
        }
    }

    /**
     * Keeps the object (and the program) alive while a callback or a wait
     * needs the changes of the pin.
     */
    void updateKeepAlive() {
        bool keep = isOpen() && (!_changeCallback.isUndefined() || _waiter.pending());
        if (keep != _kept) {
            js::keepAlive(this, keep);
            _kept = keep;
        }
    }

    /**
     * Clears the timer of a pending wait, if any.
     */
    void clearWaitTimer() {
        if (_waitTimer != 0) {
            rt::Loop::current()->clearTimer(_waitTimer);
            _waitTimer = 0;
        }
    }

    /**
     * A JS function for the timer of waitFor: it finds this object by its
     * pin (owners), never through a pointer kept by the loop.
     */
    js::Function timeoutFunction() {
        JSContext *ctx = js::context();
        JSValue pinValue = JS_NewInt32(ctx, _pin);
        JSValue function = JS_NewCFunctionData(ctx, &onWaitTimeout, 0, 0, 1, &pinValue);
        return js::Function(ctx, function);
    }

    /**
     * The timer of a waitFor ran out (JS thread): the Gpio of that pin
     * rejects its wait.
     */
    static JSValue onWaitTimeout(JSContext *ctx, JSValueConst, int, JSValueConst *, int, JSValue *data) {
        int32_t pin = 0;
        JS_ToInt32(ctx, &pin, data[0]);
        if (pin >= 0 && pin < MAX_PINS && owners[pin] != nullptr) {
            owners[pin]->waitTimedOut();
        }
        return JS_UNDEFINED;
    }

    /**
     * A change of the pin (JS thread): settles a wait for that level, then
     * calls the callback if the edge matches.
     */
    static void onMessage(void *opaque, const rt::Message *message) {
        Gpio *gpio = (Gpio *)opaque;
        int level = (int)message->value;

        // a wait for this level is over
        if (gpio->_waiter.pending() && level == gpio->_waitLevel) {
            gpio->clearWaitTimer();
            gpio->_waiter.resolve(js::Value());
        }

        // the callback, if the edge is one it wants; through a local copy:
        // the callback may replace itself or close the pin
        bool rising = level == 1;
        bool wanted = gpio->_edge == Edge::both || (gpio->_edge == Edge::rising) == rising;
        js::Function callback = gpio->_changeCallback;
        if (wanted && !callback.isUndefined()) {
            callback(level);
        }

        // nothing needs the changes any more: stop watching (the callback may
        // have closed the pin)
        if (gpio->isOpen()) {
            gpio->stopWatchingIfUnused();
            gpio->updateKeepAlive();
        }
    }

    int                     _pin = -1;            // GPIO number; -1: closed (or not initialized)
    js::Function            _changeCallback;      // onChange callback; undefined: none
    Edge                    _edge = Edge::both;   // changes that call it
    int                     _debounceMs = 0;      // debounce of onChange, ms
    js::Resolver<js::Value> _waiter;              // pending waitFor; not pending: none
    int                     _waitLevel = 0;       // level the wait waits for
    int                     _waitTimer = 0;       // timer id of its timeout; 0: none
    rt::Loop               *_loop = nullptr;      // loop of the run while watching; nullptr: not watching
    rt::SourceId            _source = { -1, 0 };  // message source of the changes
    bool                    _kept = false;        // js::keepAlive(this, true) is in force
};

} // namespace

/**
 * The "device/gpio" module: the class Gpio.
 */
JS_MODULE(gpio, module) {
    JS_ADD_CLASS(module, Gpio);
}
