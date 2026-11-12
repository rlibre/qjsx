/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The digital twin (see twin.h): the board description, the pins and the
// simulated hal::gpio.
#include "twin.h"
#include "hal_gpio.h"
#include "files.h"
#include "memory.h"
#include "platform.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "quickjs.h"

namespace twin {

namespace {

const int MAX_PINS = 64;           // pins the twin can have
const int MAX_NATIVES = 16;        // native modules a board may list

// A simulated pin (under stateLock).
struct Pin {
    PinState state;                      // what the page reads
    bool watched;                        // hal::gpio::watch() is in force
    bool partWatched;                    // a component watches it (bridge events)
    hal::gpio::ChangeHandler *handler;   // called when its level changes (watched)
    uint8_t latch;                       // what an output or open drain drives
};

// What a change of a pin calls once the lock is released.
struct Change {
    hal::gpio::ChangeHandler *handler;   // the program's handler; nullptr: none
    bool tellParts;                      // post a pinChange to the bridge
    int level;                           // the new level
};

// The board (written by load() before any thread uses it).
bool loaded = false;                 // load() succeeded
char *boardText = nullptr;           // the JSON file, owned
char *nameText = nullptr;            // name of the board, owned
char *chipText = nullptr;            // chip of the board, owned
char *platformText = nullptr;        // process.platform, owned
int pins = 0;                        // pin count
int uarts = 0;                       // UARTs of the board ("uarts")
int linkUart = -1;                   // the UART of the agent's link on the real board; -1: none

// The analog inputs of the board ("adc" of the description).
const int MAX_SPANS = 8;             // ranges of the converter a board may list
bool analogPins[MAX_PINS];           // the pins with an analog input
int converterBits = 12;              // "adc.bits"
int spans[MAX_SPANS];                // "adc.spans": readable ranges in mV, from the smallest
int spanCount = 0;                   // entries of spans
int vrefMv = 3300;                   // "adc.vref" in mV: the level 1 of a pin
char *nativeNames[MAX_NATIVES];      // owned strings
int nativeNameCount = 0;             // entries of nativeNames

// The state of the pins.
Pin pinTable[MAX_PINS];              // stateLock
platform::Mutex stateLock;           // pinTable

/**
 * The pull of a pin as a level: the program's (up or down), else a
 * component's; 0 when nothing pulls it. stateLock held.
 */
uint8_t pullLevel(const Pin *pin) {
    // the program's pull, when it configured one
    bool programPulls = pin->state.configured && pin->state.pull != (uint8_t)hal::gpio::Pull::none;
    if (programPulls) {
        return pin->state.pull == (uint8_t)hal::gpio::Pull::up ? 1 : 0;
    }

    // else a component's; floating reads 0
    return pin->state.partPull == (uint8_t)hal::gpio::Pull::up ? 1 : 0;
}

/**
 * The level of a pin from what drives it, in the order of twin.h; marks a
 * conflict between the program and a component. stateLock held.
 */
uint8_t computeLevel(Pin *pin) {
    // what the program drives: a push-pull output, or an open drain at 0
    bool output = pin->state.configured && pin->state.mode == (uint8_t)hal::gpio::Mode::output;
    bool openDrain = pin->state.configured && pin->state.mode == (uint8_t)hal::gpio::Mode::openDrain;
    int programLevel = -1;
    if (output) {
        programLevel = pin->latch;
    } else if (openDrain && pin->latch == 0) {
        programLevel = 0;
    }

    // a component driving the other level: a short on a real board
    int partLevel = pin->state.partDrive;
    pin->state.conflict = programLevel >= 0 && partLevel >= 0 && programLevel != partLevel;

    // the program, then the page, then a component, then the pulls
    if (programLevel >= 0) {
        return (uint8_t)programLevel;
    }

    if (pin->state.forced >= 0) {
        return (uint8_t)pin->state.forced;
    }

    if (partLevel >= 0) {
        return (uint8_t)partLevel;
    }

    return pullLevel(pin);
}

/**
 * The voltage on a pin in mV: what a component sets, else its level times
 * vref. stateLock held.
 */
int voltageOf(const Pin *pin) {
    const PinState *state = &pin->state;
    return state->voltageMv >= 0 ? state->voltageMv : state->level * vrefMv;
}

/**
 * Computes the level of pin 'index' again after a change; returns what to
 * call once the lock is released (the program's handler of a watched pin,
 * the components watching it), when the level changed. A change goes to
 * the analyzer. stateLock held.
 */
Change update(int index) {
    Pin *pin = &pinTable[index];
    uint8_t previous = pin->state.level;
    pin->state.level = computeLevel(pin);

    bool changed = pin->state.level != previous;
    Change change = { nullptr, false, pin->state.level };
    if (changed) {
        change.handler = pin->watched ? pin->handler : nullptr;
        change.tellParts = pin->partWatched;
        scopeRecord(index, pin->state.level, voltageOf(pin));
    }

    return change;
}

/**
 * Calls what update() returned: the program's handler, the bridge (lock
 * released).
 */
void notify(Change change, int pin) {
    if (change.handler != nullptr) {
        change.handler(pin, change.level);
    }

    if (change.tellParts) {
        uint8_t level = (uint8_t)change.level;
        bridgePost(BridgeEvent::pinChange, pin, &level, 1);
    }
}

/**
 * Copies a JS string property of 'object' (nullptr if missing).
 */
char *copyString(JSContext *ctx, JSValueConst object, const char *name) {
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    const char *text = JS_IsString(value) ? JS_ToCString(ctx, value) : nullptr;
    char *copy = text != nullptr ? memory::copyText(text, strlen(text)) : nullptr;
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, value);
    return copy;
}

/**
 * The integers of the array property 'name' of 'object' into 'out' (at most
 * 'capacity'); the count read, -1 if an item is not an integer from 'low'
 * to 'high'. A missing property reads 0 items.
 */
int readIntegers(JSContext *ctx, JSValueConst object, const char *name, int low, int high, int *out, int capacity) {
    JSValue array = JS_GetPropertyStr(ctx, object, name);
    int count = 0;
    for (uint32_t i = 0; JS_IsArray(ctx, array) && count >= 0; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, array, i);
        if (JS_IsUndefined(item)) {
            break;
        }

        int32_t value = 0;
        bool valid = JS_IsNumber(item) && JS_ToInt32(ctx, &value, item) == 0 && value >= low && value <= high;
        JS_FreeValue(ctx, item);
        if (!valid || count >= capacity) {
            count = -1;
        } else {
            out[count++] = value;
        }
    }

    JS_FreeValue(ctx, array);
    return count;
}

/**
 * Reads "adc" of the description (optional): its pins, bits, spans and
 * vref; false after printing what is wrong.
 */
bool readAdc(JSContext *ctx, JSValueConst description, const char *path) {
    JSValue adc = JS_GetPropertyStr(ctx, description, "adc");
    if (JS_IsUndefined(adc)) {
        return true;
    }

    // the pins, the spans
    int pinList[MAX_PINS];
    int pinListCount = readIntegers(ctx, adc, "pins", 0, pins - 1, pinList, MAX_PINS);
    spanCount = readIntegers(ctx, adc, "spans", 1, 100000, spans, MAX_SPANS);

    // the bits, vref (volts)
    JSValue bitsValue = JS_GetPropertyStr(ctx, adc, "bits");
    JSValue vrefValue = JS_GetPropertyStr(ctx, adc, "vref");
    int32_t bitsRead = 12;
    double vref = 3.3;
    bool bitsValid = JS_IsUndefined(bitsValue) || (JS_ToInt32(ctx, &bitsRead, bitsValue) == 0 && bitsRead >= 8 && bitsRead <= 16);
    bool vrefValid = JS_IsUndefined(vrefValue) || (JS_ToFloat64(ctx, &vref, vrefValue) == 0 && vref > 0 && vref <= 40);
    JS_FreeValue(ctx, bitsValue);
    JS_FreeValue(ctx, vrefValue);
    JS_FreeValue(ctx, adc);
    if (pinListCount < 0 || spanCount < 0 || !bitsValid || !vrefValid) {
        fprintf(stderr, "qjsx agent: %s: \"adc\" is { pins: [pin numbers], bits: 8 to 16, spans: [mV], vref: volts }\n",
                path);
        return false;
    }

    // kept
    for (int i = 0; i < pinListCount; i++) {
        analogPins[pinList[i]] = true;
    }
    converterBits = bitsRead;
    vrefMv = (int)(vref * 1000.0 + 0.5);
    return true;
}

/**
 * Reads the parsed description into the board and the pins; false after
 * printing what is wrong.
 */
bool readDescription(JSContext *ctx, JSValueConst description, const char *path) {
    // name, chip, platform and pin count
    nameText = copyString(ctx, description, "name");
    chipText = copyString(ctx, description, "chip");
    platformText = copyString(ctx, description, "platform");
    JSValue pinsValue = JS_GetPropertyStr(ctx, description, "pins");
    int32_t pinValue = 0;
    JS_ToInt32(ctx, &pinValue, pinsValue);
    JS_FreeValue(ctx, pinsValue);
    bool complete = nameText != nullptr && platformText != nullptr;
    if (!complete || pinValue < 1 || pinValue > MAX_PINS) {
        fprintf(stderr, "qjsx agent: %s: \"name\", \"platform\" and \"pins\" (1 to %d) are required\n", path,
                MAX_PINS);
        return false;
    }

    if (chipText == nullptr) {
        chipText = memory::copyText(nameText, strlen(nameText));
    }

    pins = pinValue;

    // the UARTs, and the one the real board gives to the agent's link
    int32_t uartValue = 0;
    int32_t linkValue = -1;
    JSValue uartsValue = JS_GetPropertyStr(ctx, description, "uarts");
    JSValue linkUartValue = JS_GetPropertyStr(ctx, description, "linkUart");
    bool uartsGiven = !JS_IsUndefined(uartsValue);
    bool linkGiven = !JS_IsUndefined(linkUartValue);
    JS_ToInt32(ctx, &uartValue, uartsValue);
    JS_ToInt32(ctx, &linkValue, linkUartValue);
    JS_FreeValue(ctx, uartsValue);
    JS_FreeValue(ctx, linkUartValue);
    bool uartsValid = !uartsGiven || (uartValue >= 0 && uartValue <= MAX_UARTS);
    bool linkValid = !linkGiven || (linkValue >= 0 && linkValue < uartValue);
    if (!uartsValid || !linkValid) {
        fprintf(stderr, "qjsx agent: %s: \"uarts\" is 0 to %d, \"linkUart\" one of them\n", path, MAX_UARTS);
        return false;
    }

    uarts = uartsGiven ? uartValue : 0;
    linkUart = linkGiven ? linkValue : -1;

    // the native modules of the board
    JSValue nativesValue = JS_GetPropertyStr(ctx, description, "natives");
    for (uint32_t i = 0; JS_IsArray(ctx, nativesValue) && nativeNameCount < MAX_NATIVES; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, nativesValue, i);
        const char *name = JS_IsString(item) ? JS_ToCString(ctx, item) : nullptr;
        JS_FreeValue(ctx, item);
        if (name == nullptr) {
            break;
        }

        nativeNames[nativeNameCount++] = memory::copyText(name, strlen(name));
        JS_FreeCString(ctx, name);
    }

    // the parts are the components' business (host/twin/js): their models
    // drive and pull the pins
    JS_FreeValue(ctx, nativesValue);

    // the analog inputs
    return readAdc(ctx, description, path);
}

} // namespace

/**
 * 1. read the file;
 * 2. parse it with a short-lived QuickJS context;
 * 3. read the board, the natives, the parts;
 * 4. the pins at rest.
 */
bool load(const char *path) {
    // the file
    size_t size = 0;
    uint8_t *text = files::readAll(path, &size);
    if (text == nullptr) {
        fprintf(stderr, "qjsx agent: cannot read the board %s: %s\n", path, strerror(errno));
        return false;
    }

    boardText = (char *)memory::allocate(size + 1);
    memcpy(boardText, text, size);
    boardText[size] = 0;
    memory::release(text);

    // parse it
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(runtime);
    JSValue description = JS_ParseJSON(ctx, boardText, size, path);
    bool ok = JS_IsObject(description);
    if (!ok) {
        fprintf(stderr, "qjsx agent: %s is not a JSON object\n", path);
    }

    // read it
    ok = ok && readDescription(ctx, description, path);
    JS_FreeValue(ctx, description);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    if (!ok) {
        return false;
    }

    // the pins at rest: nothing forced, nothing driven by a component; the
    // analyzer starts from there
    scopeStart();
    for (int i = 0; i < pins; i++) {
        pinTable[i].state.forced = -1;
        pinTable[i].state.partDrive = -1;
        pinTable[i].state.voltageMv = -1;
        pinTable[i].state.level = computeLevel(&pinTable[i]);
        scopeSetInitial(i, pinTable[i].state.level, voltageOf(&pinTable[i]));
    }

    loaded = true;
    return true;
}

/**
 * load() succeeded.
 */
bool active() {
    return loaded;
}

/**
 * The description, as read.
 */
const char *boardJson() {
    return boardText;
}

/**
 * Name of the board.
 */
const char *name() {
    return nameText;
}

/**
 * Chip of the board.
 */
const char *chip() {
    return chipText;
}

/**
 * process.platform of the board.
 */
const char *platform() {
    return platformText;
}

/**
 * Number of pins.
 */
int pinCount() {
    return pins;
}

/**
 * UARTs of the board.
 */
int uartCount() {
    return uarts;
}

/**
 * The UART of the agent's link on the real board; -1: none.
 */
int linkUartNumber() {
    return linkUart;
}

/**
 * The native modules of the board.
 */
const char *const *natives() {
    return nativeNames;
}

/**
 * How many native modules the board has.
 */
int nativeCount() {
    return nativeNameCount;
}

/**
 * Copies the pins' states.
 */
void snapshot(PinState *states, int count) {
    stateLock.lock();
    for (int i = 0; i < count && i < pins; i++) {
        states[i] = pinTable[i].state;
    }

    stateLock.unlock();
}

/**
 * The page forces a pin (or lets it go).
 */
void force(int pin, int level) {
    if (pin < 0 || pin >= pins) {
        return;
    }

    stateLock.lock();
    pinTable[pin].state.forced = (int8_t)(level < 0 ? -1 : (level != 0 ? 1 : 0));
    Change change = update(pin);
    stateLock.unlock();

    notify(change, pin);
}

/**
 * A component drives a pin (or lets it go).
 */
void partDrive(int pin, int level) {
    if (pin < 0 || pin >= pins) {
        return;
    }

    stateLock.lock();
    pinTable[pin].state.partDrive = (int8_t)(level < 0 ? -1 : (level != 0 ? 1 : 0));
    Change change = update(pin);
    stateLock.unlock();

    notify(change, pin);
}

/**
 * A component pulls a pin up or down, or not any more.
 */
void partPull(int pin, int pull) {
    if (pin < 0 || pin >= pins) {
        return;
    }

    stateLock.lock();
    pinTable[pin].state.partPull = (uint8_t)(pull == 1 || pull == 2 ? pull : 0);
    Change change = update(pin);
    stateLock.unlock();

    notify(change, pin);
}

/**
 * A component watches a pin, or not any more.
 */
void partWatch(int pin, bool watched) {
    if (pin < 0 || pin >= pins) {
        return;
    }

    stateLock.lock();
    pinTable[pin].partWatched = watched;
    stateLock.unlock();
}

/**
 * A channel of hal::rmt takes or lets go a pin.
 */
void setRmt(int pin, bool driven) {
    if (pin < 0 || pin >= pins) {
        return;
    }

    stateLock.lock();
    pinTable[pin].state.rmt = driven;
    stateLock.unlock();
}

/**
 * Whether a pin has an analog input.
 */
bool adcPin(int pin) {
    return pin >= 0 && pin < pins && analogPins[pin];
}

/**
 * Resolution of the converter.
 */
int adcBits() {
    return converterBits;
}

/**
 * The span read for a range: the first listed span that covers it, else
 * the largest up to vref; 0 above vref.
 */
int adcSpanFor(int rangeMv) {
    if (rangeMv <= 0 || rangeMv > vrefMv) {
        return 0;
    }

    for (int i = 0; i < spanCount; i++) {
        if (rangeMv <= spans[i]) {
            return spans[i];
        }
    }
    return spanCount > 0 ? spans[spanCount - 1] : vrefMv;
}

/**
 * The voltage on a pin: a component's, else its level times vref.
 */
int pinVoltage(int pin) {
    if (pin < 0 || pin >= pins) {
        return 0;
    }

    stateLock.lock();
    int millivolts = voltageOf(&pinTable[pin]);
    stateLock.unlock();
    return millivolts;
}

/**
 * The program opens or closes the analog input of a pin.
 */
void setAdc(int pin, bool open) {
    if (pin < 0 || pin >= pins) {
        return;
    }

    stateLock.lock();
    pinTable[pin].state.adc = open;
    stateLock.unlock();
}

/**
 * A component sets the voltage on a pin (at most vref), or not any more; a
 * change of the voltage goes to the analyzer.
 */
void partVoltage(int pin, int millivolts) {
    if (pin < 0 || pin >= pins) {
        return;
    }

    int bounded = millivolts < 0 ? -1 : (millivolts > vrefMv ? vrefMv : millivolts);
    stateLock.lock();

    // the new voltage
    Pin *target = &pinTable[pin];
    int previous = voltageOf(target);
    target->state.voltageMv = (int16_t)bounded;

    // to the analyzer when it changed
    int now = voltageOf(target);
    if (now != previous) {
        scopeRecord(pin, target->state.level, now);
    }

    stateLock.unlock();
}

/**
 * A bus of hal::i2c takes or lets go a pin.
 */
void setI2c(int pin, bool used) {
    if (pin < 0 || pin >= pins) {
        return;
    }

    stateLock.lock();
    pinTable[pin].state.i2c = used;
    stateLock.unlock();
}

} // namespace twin

// ### hal::gpio, simulated #####################################################

namespace hal {
namespace gpio {

/**
 * The pins of the board.
 */
int pinCount() {
    return twin::pinCount();
}

/**
 * The program takes the pin: mode, pull; an output starts at 0.
 */
bool configure(int pin, Mode mode, Pull pull) {
    if (pin < 0 || pin >= twin::pinCount()) {
        return false;
    }

    twin::stateLock.lock();
    twin::Pin *target = &twin::pinTable[pin];
    target->state.configured = true;
    target->state.mode = (uint8_t)mode;
    target->state.pull = (uint8_t)pull;
    target->latch = 0;
    twin::Change change = twin::update(pin);
    twin::stateLock.unlock();

    twin::notify(change, pin);
    return true;
}

/**
 * The level on the line.
 */
int read(int pin) {
    twin::stateLock.lock();
    int level = twin::pinTable[pin].state.level;
    twin::stateLock.unlock();
    return level;
}

/**
 * Sets what the output drives.
 */
void write(int pin, int level) {
    twin::stateLock.lock();
    twin::pinTable[pin].latch = level != 0 ? 1 : 0;
    twin::Change change = twin::update(pin);
    twin::stateLock.unlock();

    twin::notify(change, pin);
}

/**
 * Inverts what the output drives.
 */
void toggle(int pin) {
    twin::stateLock.lock();
    twin::pinTable[pin].latch ^= 1;
    twin::Change change = twin::update(pin);
    twin::stateLock.unlock();

    twin::notify(change, pin);
}

/**
 * Watches the level of a pin: each change calls 'handler' at once, on the
 * thread that made it (no debounce: the twin's lines do not bounce).
 */
bool watch(int pin, int, ChangeHandler *handler) {
    twin::stateLock.lock();
    twin::pinTable[pin].watched = true;
    twin::pinTable[pin].handler = handler;
    twin::stateLock.unlock();
    return true;
}

/**
 * Stops watching.
 */
void unwatch(int pin) {
    twin::stateLock.lock();
    twin::pinTable[pin].watched = false;
    twin::stateLock.unlock();
}

/**
 * Not watched, not configured any more; the components watching the pin
 * hear of a change of its level (the program's handler is gone).
 */
void reset(int pin) {
    twin::stateLock.lock();
    twin::Pin *target = &twin::pinTable[pin];
    target->watched = false;
    target->state.configured = false;
    target->latch = 0;
    twin::Change change = twin::update(pin);
    twin::stateLock.unlock();

    twin::notify(change, pin);
}

} // namespace gpio
} // namespace hal
