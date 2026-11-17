/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// An integer argument or option of a device module, checked strictly as the
// "Errors of the JS API" ask (user, 2026-11-17): anything but a number is a
// TypeError; a number that is not an integer (a fraction, NaN, Infinity) or
// is outside [Lo, Hi] is a RangeError (js::Range would give a TypeError for
// a fraction or NaN). Used like an int:
//
//   void init(device::StrictInt<0, 255> pin);   // (int)pin: the integer
//
// Shared by the modules that follow those rules (spi_module.cpp,
// dac_module.cpp); the others will come in the later pass through jsbind.
#pragma once

#include <math.h>
#include <stdio.h>

#include "jsbind.h"

namespace device {

template<int Lo, int Hi>
struct StrictInt {
    int value;   // the integer

    /**
     * Zero (not checked against the bounds).
     */
    StrictInt() : value(0) {}

    /**
     * Given value (a default, not checked against the bounds).
     */
    StrictInt(int initial) : value(initial) {}

    /**
     * Reads the integer.
     */
    operator int() const {
        return value;
    }
};

} // namespace device

namespace js {

template<int Lo, int Hi>
struct Convert<device::StrictInt<Lo, Hi>> {
    /**
     * Accepts a number that is an integer within [Lo, Hi]: TypeError for
     * anything but a number, RangeError for the other numbers.
     */
    static bool from(JSContext *ctx, JSValueConst value, device::StrictInt<Lo, Hi> &out, const detail::Where &where) {
        // a number: no implicit conversion from strings
        double number = 0;
        bool isNumber = JS_IsNumber(value) && JS_ToFloat64(ctx, &number, value) >= 0;
        if (!isNumber) {
            return detail::conversionError(where, "must be an integer");
        }

        // an integer: finite, with no fractional part
        bool isInteger = isfinite(number) && number == floor(number);
        if (!isInteger) {
            return detail::conversionError<RangeError>(where, "must be an integer (not a fraction, NaN or Infinity)");
        }

        // within the bounds
        bool inBounds = number >= (double)Lo && number <= (double)Hi;
        if (!inBounds) {
            char expectation[96];
            snprintf(expectation, sizeof(expectation), "must be between %d and %d", Lo, Hi);
            return detail::conversionError<RangeError>(where, expectation);
        }

        out.value = (int)number;
        return true;
    }

    /**
     * JS number.
     */
    static JSValue to(JSContext *ctx, const device::StrictInt<Lo, Hi> &value) {
        return JS_NewInt32(ctx, value.value);
    }
};

} // namespace js
