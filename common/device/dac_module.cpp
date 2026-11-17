/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "device/dac": the analog outputs of the board (hal_dac.h), on the
// boards whose chip has a DAC (registered by the port, see
// device_modules.h).
//
//   import { Dac } from "device/dac";
//   const out = new Dac(25, { value: 0 });   // GPIO25, starts at code 0
//   out.write(128);                          // a raw code, 0 to 2^bits - 1
//   out.writeVoltage(1.2);                   // volts, rounded to the nearest code
//   out.bits; out.fullScale; out.value;      // 8, 3.3 (volts of the highest code), 93
//   out.close();
//
// Everything is synchronous. Code c gives c / (2^bits - 1) of the full
// scale. Closed, or at the end of the program (the finalizer), the output
// stops and the pin is given back (user, 2026-11-17: like Pwm).
//
// Errors as the "Errors of the JS API" ask: a wrong type is a TypeError, a
// number out of range or not an integer a RangeError (no code); EINVAL (no
// DAC on the pin), EBUSY (open already), EBADF (closed), EIO (the hardware
// refused it).
#include <errno.h>
#include <math.h>
#include <stdio.h>

#include "jsbind.h"
#include "hal_dac.h"
#include "strict_int.h"

namespace {

using device::StrictInt;

// The widest code a DAC may take (16 bits); the real bound, 2^bits - 1, is
// checked once the output is open.
const int HIGHEST_CODE = 65535;
typedef StrictInt<0, HIGHEST_CODE> Code;

// Options of 'new Dac(pin, options)'.
struct DacOptions {
    Code value = 0;                              // the first code, 0 to 2^bits - 1
};
JS_STRUCT(DacOptions, value);

/**
 * An analog output (JS class Dac).
 */
class Dac {
    JS_CLASS(Dac);

public:
    /**
     * Finalizer (no JS here): stops the output.
     */
    ~Dac() {
        closeOutput();
    }

    /**
     * JS constructor: new Dac(pin, { value }) opens the DAC of the pin and
     * drives its first code. Throws a TypeError / RangeError (the
     * arguments), EINVAL (no DAC on the pin), EBUSY (open already) or EIO.
     */
    void init(StrictInt<0, 255> pin, js::Optional<DacOptions> options) {
        // the options (their types checked by the conversion)
        DacOptions settings = options.has() ? options.value() : DacOptions();

        // the output
        hal::dac::Opened opened = hal::dac::open((int)pin);
        if (opened.handle < 0) {
            char what[48];
            snprintf(what, sizeof(what), "dac: pin %d", (int)pin);
            return (void)js::failSystem(opened.error, what);
        }

        // opened: what the hardware has, in the fields
        _handle = opened.handle;
        bits = opened.bits;
        fullScale = opened.fullScaleMv / 1000.0;

        // the first code, within the resolution; else the output goes again
        if (!isCode((int)settings.value)) {
            closeOutput();
            return (void)js::fail<js::RangeError>("dac: value must be an integer 0 to %d", highestCode());
        }

        // driven; a refusal of the hardware closes it again
        if (!drive((int)settings.value)) {
            closeOutput();
        }
    }

    /**
     * JS: drives a raw code, 0 to 2^bits - 1. Throws a RangeError (not an
     * integer of that range) or EBADF (closed).
     */
    JS_EXPORT(write) void write(Code raw) {
        // closed: no output
        if (!isOpen()) {
            return (void)js::failSystem(EBADF, "dac: closed");
        }

        // a code of the resolution
        if (!isCode((int)raw)) {
            return (void)js::fail<js::RangeError>("dac: the code must be an integer 0 to %d", highestCode());
        }

        drive((int)raw);
    }

    /**
     * JS: drives a voltage, 0 to fullScale volts, rounded to the nearest
     * code. Throws a RangeError (outside, NaN) or EBADF (closed).
     */
    JS_EXPORT(writeVoltage) void writeVoltage(double volts) {
        // closed: no output
        if (!isOpen()) {
            return (void)js::failSystem(EBADF, "dac: closed");
        }

        // within the full scale (false for NaN too)
        bool inRange = volts >= 0.0 && volts <= fullScale;
        if (!inRange) {
            return (void)js::fail<js::RangeError>("dac: the voltage must be 0 to %g volts", fullScale);
        }

        // the nearest code: the share of the full scale, on the codes
        double share = volts / fullScale;
        int raw = (int)llround(share * highestCode());
        drive(raw);
    }

    /**
     * JS: stops the output and gives the pin back; the writes throw EBADF
     * after it. Closing twice does nothing.
     */
    JS_EXPORT(close) void close() {
        closeOutput();
    }

    // What the hardware has and does (set when opened and by each write;
    // read-only for the program: writing them changes nothing).
    JS_EXPORT(bits) int bits = 0;                // resolution: codes 0 to 2^bits - 1
    JS_EXPORT(fullScale) double fullScale = 0;   // volts of the highest code
    JS_EXPORT(value) int value = 0;              // the code written last

private:
    /**
     * Whether the output is open.
     */
    bool isOpen() const {
        return _handle >= 0;
    }

    /**
     * The highest code of the resolution: 2^bits - 1.
     */
    int highestCode() const {
        return (1 << bits) - 1;
    }

    /**
     * Whether 'raw' is a code of the resolution.
     */
    bool isCode(int raw) const {
        return raw >= 0 && raw <= highestCode();
    }

    /**
     * Gives a code (checked) to the hal and keeps it in 'value'; false with
     * the error recorded when the hardware refuses it.
     */
    bool drive(int raw) {
        // to the hardware
        int error = hal::dac::write(_handle, raw);
        if (error != 0) {
            js::failSystem(error, "dac: write");
            return false;
        }

        // what it does now
        value = raw;
        return true;
    }

    /**
     * Stops the output, once.
     */
    void closeOutput() {
        if (_handle >= 0) {
            hal::dac::close(_handle);
            _handle = -1;
        }
    }

    int _handle = -1;            // of hal::dac; -1: closed
};

} // namespace

/**
 * Module "device/dac".
 */
JS_MODULE(dac, module) {
    JS_ADD_CLASS(module, Dac);
}
