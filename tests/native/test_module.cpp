/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Native module "jsbind_test": exercises the binding library from JS
// (tests/jsbind.test.js). Only built into qjsx-tests.
#include <errno.h>

#include "jsbind.h"
#include "modules.h"

namespace {

JS_ENUM(Parity, none, odd, even);

using Speed = js::Range<int, 300, 4000000>;

struct DeviceOptions {
    Speed speed = 115200;           // bits per second
    Parity parity = Parity::none;   // JS: "none", "odd" or "even"
    bool echo = false;              // fake flag, only stored
};
JS_STRUCT(DeviceOptions, speed, parity, echo);

int destroyedDevices = 0;

/**
 * A fake device (JS class Device) exercising every feature of the binding:
 * constructor arguments, fields, options structs, enums, ranges, callbacks,
 * promises, errors, byte arrays, native objects as arguments and results,
 * keep-alive.
 */
class Device {
    JS_CLASS(Device);

public:
    /**
     * Counts the destroyed objects (destroyedCount()), to test the finalizer.
     */
    ~Device() {
        destroyedDevices++;
    }

    /**
     * JS constructor: new Device(port), port 0 or 1 (js::Range).
     */
    void init(js::Range<int, 0, 1> port) {
        portNumber = port;
    }

    JS_EXPORT(name) const char *const name = "device";   // read-only JS property (const)
    JS_EXPORT(speed) int speed = 0;                      // bits per second, set by open(); 0: never opened
    JS_EXPORT(parity) Parity parity = Parity::none;      // set by open()
    JS_EXPORT(portNumber) int portNumber = -1;           // 0 or 1, given to the constructor

    /**
     * Opens with the given options (a JS_STRUCT) and keeps the object
     * alive; fails if already open.
     */
    JS_EXPORT(open) void open(DeviceOptions options) {
        // a second open fails
        if (isOpen) {
            return (void)js::fail("already open");
        }

        // take the options; an open device stays alive until close()
        isOpen = true;
        speed = options.speed;
        parity = options.parity;
        echo = options.echo;
        js::keepAlive(this, true);
    }

    /**
     * Closes and drops the keep-alive.
     */
    JS_EXPORT(close) void close() {
        isOpen = false;
        js::keepAlive(this, false);
    }

    /**
     * Returns 'A', or fails if not open (js::fail converted to an int
     * result).
     */
    JS_EXPORT(readChar) int readChar() {
        if (!isOpen) {
            return js::fail("port not open");
        }

        return 'A';
    }

    /**
     * Stores the callback called by emit().
     */
    JS_EXPORT(onData) void onData(js::Function callback) {
        dataCallback = callback;
    }

    /**
     * Calls the JS callback synchronously; a JS exception propagates.
     */
    JS_EXPORT(emit) bool emit(js::Str text) {
        dataCallback(text);
        return true;
    }

    /**
     * A promise settled later by resolvePending() or rejectPending().
     */
    JS_EXPORT(readLater) js::Promise<js::Str> readLater() {
        js::Promise<js::Str> promise = js::Promise<js::Str>::create();
        pending = promise.resolver();
        return promise;
    }

    /**
     * Resolves the promise of readLater() with 'text'.
     */
    JS_EXPORT(resolvePending) void resolvePending(js::Str text) {
        pending.resolve(text);
    }

    /**
     * Rejects the promise of readLater() with a RangeError whose message is
     * 'reason'.
     */
    JS_EXPORT(rejectPending) void rejectPending(js::Str reason) {
        pending.reject<js::RangeError>("%.*s", (int)(reason.size()), reason.data());
    }

    /**
     * True while the promise of readLater() is not settled.
     */
    JS_EXPORT(isPending) bool isPending() const {
        return pending.pending();
    }

    /**
     * Sum of all the arguments (js::Args: any number of them).
     */
    JS_EXPORT(sum) double sum(js::Args numbers) {
        double total = 0;

        for (int i = 0; i < numbers.count; i++) {
            double value = 0;
            JS_ToFloat64(numbers.ctx, &value, numbers[i]);
            total += value;
        }

        return total;
    }

    /**
     * Size of a byte view (js::Bytes).
     */
    JS_EXPORT(byteCount) int byteCount(js::Bytes bytes) {
        return (int)(bytes.size());
    }

    /**
     * A new Uint8Array of 'size' bytes 0, 1, 2... (js::Buffer).
     */
    JS_EXPORT(makeBytes) js::Buffer makeBytes(js::Range<int, 0, 1024> size) {
        js::Buffer buffer = js::Buffer::create(size);

        for (int i = 0; i < size; i++) {
            buffer.data()[i] = (uint8_t)i;
        }

        return buffer;
    }

    /**
     * Always fails with ENOENT (js::failSystem: a Node-like error.code).
     */
    JS_EXPORT(systemError) void systemError() {
        return (void)js::failSystem(ENOENT, "test");
    }

    /**
     * True if 'device' is another Device (a native object as argument).
     */
    JS_EXPORT(isOther) bool isOther(Device *device) {
        return device != this;
    }

    /**
     * Returns this object (a native object as result).
     */
    JS_EXPORT(self) Device *self() {
        return this;
    }

    /**
     * The current options, returned as a struct (converted to an object).
     */
    JS_EXPORT(options) DeviceOptions options() const {
        DeviceOptions current;
        current.speed = speed;
        current.parity = parity;
        current.echo = echo;
        return current;
    }

    /**
     * Two errors in one call: the first one wins.
     */
    JS_EXPORT(twoErrors) void twoErrors() {
        js::fail<js::TypeError>("first");
        js::fail<js::RangeError>("second");
    }

private:
    js::Function dataCallback;       // onData callback, called by emit()
    js::Resolver<js::Str> pending;   // promise of readLater(), settled by resolvePending / rejectPending
    bool isOpen = false;             // true between open() and close()
    bool echo = false;               // from the options of open()
};

/**
 * a + b (a plain function with converted arguments).
 */
int add(int a, int b) {
    return a + b;
}

/**
 * Runs the garbage collector, so that tests can check the finalizers.
 */
void gc() {
    JS_RunGC(JS_GetRuntime(js::context()));
}

/**
 * Number of Device objects destroyed so far.
 */
int destroyedCount() {
    return destroyedDevices;
}

/**
 * Number of native objects currently kept alive (js::keepAlive).
 */
int keptAliveCount() {
    return js::keptAliveCount();
}

} // namespace

/**
 * Module "jsbind_test": the class Device and the functions above.
 */
JS_MODULE(jsbind_test, module) {
    JS_ADD_CLASS(module, Device);
    JS_FUNCTION(module, add);
    JS_FUNCTION(module, gc);
    JS_FUNCTION(module, destroyedCount);
    JS_FUNCTION(module, keptAliveCount);
}

/**
 * Registers the module "debug_test".
 */
void registerDebugTestModule();   // debug_module.cpp

/**
 * Registers the native test modules; called by main() in qjsx-tests only.
 */
void registerTestModules() {
    rt::registerNativeModule("jsbind_test", &jsNewModule_jsbind_test);
    registerDebugTestModule();
}
