/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "device/adc": the analog inputs of the board (hal_adc.h), on the
// boards whose port implements it (registered by the port, see
// device_modules.h).
//
//   import { Adc } from "device/adc";
//   const pot = new Adc(4, { range: 3.3 });   // GPIO4; the highest voltage expected, in volts
//   pot.read();          // raw value, 0 to 4095 on 12 bits
//   pot.readVoltage();   // volts, corrected by the calibration of the chip
//   pot.bits;            // resolution: 12
//   pot.close();
//
// A read is synchronous (a few microseconds). A pin without an analog input
// is refused (EINVAL), as a range the converter cannot read.
#include <errno.h>

#include "jsbind.h"
#include "hal_adc.h"

namespace {

// Options of 'new Adc(pin, options)'.
struct AdcOptions {
    double range = 3.3;                          // the highest voltage expected, in volts (0.1 to 40)
};
JS_STRUCT(AdcOptions, range);

/**
 * An analog input (JS class Adc).
 */
class Adc {
    JS_CLASS(Adc);

public:
    /**
     * Finalizer (no JS here): closes the input.
     */
    ~Adc() {
        closeInput();
    }

    /**
     * JS constructor: new Adc(pin, { range }) opens the analog input of the
     * pin. Throws EINVAL (no analog input there, or a range it cannot read)
     * or EBUSY (already open).
     */
    void init(js::Range<int, 0, 63> pin, js::Optional<AdcOptions> options) {
        // the range asked, checked
        AdcOptions settings = options.has() ? options.value() : AdcOptions();
        if (!(settings.range >= 0.1 && settings.range <= 40.0)) {
            return (void)js::fail<js::RangeError>("adc: range must be 0.1 to 40 volts");
        }

        // the input, for that range
        int rangeMv = (int)(settings.range * 1000.0 + 0.5);
        hal::adc::Opened opened = hal::adc::open((int)pin, rangeMv);
        if (opened.handle < 0) {
            return (void)js::failSystem(opened.error, "adc: no analog input on that pin for that range");
        }

        _handle = opened.handle;
        bits = opened.bits;
    }

    /**
     * JS: a raw conversion, 0 to 2^bits - 1. Throws if closed or if the
     * conversion fails (EIO).
     */
    JS_EXPORT(read) int read() {
        if (!isOpen()) {
            return js::fail("adc: closed");
        }

        int raw = hal::adc::readRaw(_handle);
        if (raw < 0) {
            return js::failSystem(EIO, "adc: read");
        }
        return raw;
    }

    /**
     * JS: a conversion in volts, corrected by the calibration of the chip.
     * Throws if closed or if the conversion fails (EIO).
     */
    JS_EXPORT(readVoltage) double readVoltage() {
        if (!isOpen()) {
            return js::fail("adc: closed");
        }

        int millivolts = hal::adc::readMillivolts(_handle);
        if (millivolts < 0) {
            return js::failSystem(EIO, "adc: readVoltage");
        }
        return millivolts / 1000.0;
    }

    /**
     * JS: closes the input; reads throw after it. Closing twice does nothing.
     */
    JS_EXPORT(close) void close() {
        closeInput();
    }

    // The resolution of the raw values, in bits (set when opened; writing it
    // changes nothing).
    JS_EXPORT(bits) int bits = 0;

private:
    /**
     * Whether the input is open.
     */
    bool isOpen() const {
        return _handle >= 0;
    }

    /**
     * Closes the input, once.
     */
    void closeInput() {
        if (_handle >= 0) {
            hal::adc::close(_handle);
            _handle = -1;
        }
    }

    int _handle = -1;            // of hal::adc; -1: closed
};

} // namespace

/**
 * Module "device/adc".
 */
JS_MODULE(adc, module) {
    JS_ADD_CLASS(module, Adc);
}
