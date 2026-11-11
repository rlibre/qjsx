/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Debugger stub: JS values on the wire (encoding described in
// host/js/protocol.js, VALUE). Objects stay on the device behind handles;
// they are described without running JS code: no getter, no proxy trap.
// Exception: the 'source' and 'flags' of a RegExp come from the built-in
// accessors of RegExp.prototype.
#include "debugger.h"
#include "memory.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "protocol.h"

namespace agent {

namespace {

// Longest string sent in a value; the host shows longer ones cut.
const size_t MAX_STRING = 10000;
// Most properties sent for one object (large arrays: the host asks for
// ranges with its own helper functions).
const int MAX_PROPERTIES = 1000;

/**
 * Text of a C string, or "" if null.
 */
const char *orEmpty(const char *text) {
    return text != nullptr ? text : "";
}

/**
 * malloc'd "<text>(<number>)", like "Array(3)".
 */
char *formatWithCount(const char *text, int64_t number) {
    size_t size = strlen(text) + 32;
    char *result = (char *)memory::allocate(size);
    snprintf(result, size, "%s(%" PRId64 ")", text, number);
    return result;
}

/**
 * malloc'd "function <name>()".
 */
char *formatFunction(const char *name) {
    size_t size = strlen(name) + 16;
    char *result = (char *)memory::allocate(size);
    snprintf(result, size, "function %s()", name);
    return result;
}

/**
 * malloc'd copy of a zero-terminated string.
 */
char *copyString(const char *text) {
    return memory::copyText(text, strlen(text));
}

/**
 * Own data property 'name' of 'object' (new reference), JS_UNDEFINED if it
 * is missing or an accessor: never runs a getter.
 */
JSValue ownDataProperty(JSContext *ctx, JSValueConst object, const char *name) {
    // read the own descriptor
    JSAtom atom = JS_NewAtom(ctx, name);
    JSPropertyDescriptor descriptor;
    int found = JS_GetOwnProperty(ctx, &descriptor, object, atom);
    JS_FreeAtom(ctx, atom);

    // an error is cleared and gives undefined
    if (found < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }

    // missing: undefined
    if (found == 0) {
        return JS_UNDEFINED;
    }

    // the getter and setter are not needed
    JS_FreeValue(ctx, descriptor.getter);
    JS_FreeValue(ctx, descriptor.setter);

    // an accessor gives undefined (its getter is not called)
    bool isAccessor = (descriptor.flags & JS_PROP_GETSET) != 0;
    if (isAccessor) {
        JS_FreeValue(ctx, descriptor.value);
        return JS_UNDEFINED;
    }

    // a data property: the reference of its value passes to the caller
    return descriptor.value;
}

/**
 * malloc'd copy of an own string data property, or nullptr.
 */
char *ownStringProperty(JSContext *ctx, JSValueConst object, const char *name) {
    JSValue value = ownDataProperty(ctx, object, name);
    char *result = nullptr;
    if (JS_IsString(value)) {
        const char *text = JS_ToCString(ctx, value);
        result = copyString(orEmpty(text));
        JS_FreeCString(ctx, text);
    }

    JS_FreeValue(ctx, value);
    return result;
}

/**
 * Integer division rounded toward minus infinity (C++ rounds toward zero).
 */
int64_t floorDivide(int64_t value, int64_t divisor) {
    int64_t quotient = value / divisor;
    bool hasRemainder = value % divisor != 0;
    bool isNegative = (value < 0) != (divisor < 0);
    if (hasRemainder && isNegative) {
        quotient--;
    }

    return quotient;
}

/**
 * Proleptic Gregorian date of a day number (days since 1970-01-01, negative
 * before), computed without gmtime: "civil_from_days" of Howard Hinnant.
 */
void civilFromDays(int64_t days, int64_t *year, int *month, int *day) {
    // count the days from 0000-03-01, in eras of 400 years (146097 days):
    // starting in March puts the leap day at the end of the year
    int64_t shifted = days + 719468;
    int64_t era = floorDivide(shifted, 146097);
    int64_t dayOfEra = shifted - era * 146097;   // 0..146096

    // the year in the era: the days without the leap days (every 4 years,
    // except every 100, except every 400) divided by 365
    int64_t leapDaysCorrection = dayOfEra / 1460 - dayOfEra / 36524 + dayOfEra / 146096;
    int64_t yearOfEra = (dayOfEra - leapDaysCorrection) / 365;   // 0..399
    int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);   // 0..365, from March 1st

    // month and day, from March (0) to February (11): the months follow a
    // 153-day pattern every 5 months
    int64_t monthFromMarch = (5 * dayOfYear + 2) / 153;
    *day = (int)(dayOfYear - (153 * monthFromMarch + 2) / 5 + 1);
    *month = (int)(monthFromMarch < 10 ? monthFromMarch + 3 : monthFromMarch - 9);

    // January and February belong to the next calendar year
    bool isJanuaryOrFebruary = *month <= 2;
    *year = era * 400 + yearOfEra + (isJanuaryOrFebruary ? 1 : 0);
}

/**
 * A Date time value as ISO 8601 text, like toISOString ("Invalid Date" for
 * NaN). malloc'd.
 */
char *formatDate(double timeValue) {
    // a valid time value is a whole number of milliseconds within 8.64e15 of
    // 1970 (TimeClip); the test is false for NaN too
    const double MAX_TIME = 8.64e15;
    bool isValid = timeValue >= -MAX_TIME && timeValue <= MAX_TIME;
    if (!isValid) {
        return copyString("Invalid Date");
    }

    // split into days since 1970 and milliseconds of the day, rounded toward
    // minus infinity: before 1970 the milliseconds of the day stay positive
    const int64_t MS_PER_DAY = 86400000;
    int64_t time = (int64_t)timeValue;
    int64_t days = floorDivide(time, MS_PER_DAY);
    int64_t msOfDay = time - days * MS_PER_DAY;

    // the time of day
    int hours = (int)(msOfDay / 3600000);
    int minutes = (int)(msOfDay / 60000 % 60);
    int seconds = (int)(msOfDay / 1000 % 60);
    int milliseconds = (int)(msOfDay % 1000);

    // the calendar date
    int64_t year = 0;
    int month = 0;
    int day = 0;
    civilFromDays(days, &year, &month, &day);

    // the year like toISOString: 4 digits from 0 to 9999, else a sign and 6
    // digits ("-000001", "+275760")
    char yearText[16];
    bool isExtendedYear = year < 0 || year > 9999;
    if (isExtendedYear) {
        char sign = year < 0 ? '-' : '+';
        int64_t magnitude = year < 0 ? -year : year;
        snprintf(yearText, sizeof(yearText), "%c%06" PRId64, sign, magnitude);
    } else {
        snprintf(yearText, sizeof(yearText), "%04" PRId64, year);
    }

    // formatted as 2026-01-31T12:00:00.000Z
    char text[64];
    snprintf(text, sizeof(text), "%s-%02d-%02dT%02d:%02d:%02d.%03dZ", yearText, month, day, hours, minutes,
             seconds, milliseconds);
    return copyString(text);
}

/**
 * Writes a text, cut to MAX_STRING bytes (the cut may fall inside a UTF-8
 * sequence).
 */
void writeText(cbor::Writer *writer, const char *text, size_t length) {
    writer->text(text, length < MAX_STRING ? length : MAX_STRING);
}

} // namespace

// ### Writing values ##########################################################

/**
 * Writes 'value' in the VALUE encoding of the protocol; objects and symbols
 * get a handle in 'group'.
 */
void Debugger::writeValue(cbor::Writer *writer, JSValueConst value, int group) {
    // by tag: undefined, null, booleans, integers and strings inline
    int tag = JS_VALUE_GET_TAG(value);

    switch (tag) {
        case JS_TAG_UNDEFINED: {
            writer->undefined();
            return;
        }
        case JS_TAG_NULL: {
            writer->null();
            return;
        }
        case JS_TAG_BOOL: {
            writer->boolean(JS_ToBool(_ctx, value) != 0);
            return;
        }
        case JS_TAG_INT: {
            writer->integer(JS_VALUE_GET_INT(value));
            return;
        }
        case JS_TAG_UNINITIALIZED: {
            // temporal dead zone: [VALUE_UNINITIALIZED]
            writer->array(1);
            writer->integer(VALUE_UNINITIALIZED);
            return;
        }
        case JS_TAG_STRING: {
            // cut to MAX_STRING
            size_t length = 0;
            const char *text = JS_ToCStringLen(_ctx, &length, value);
            writeText(writer, orEmpty(text), text != nullptr ? length : 0);
            JS_FreeCString(_ctx, text);
            return;
        }
        case JS_TAG_SYMBOL: {
            // [VALUE_SYMBOL, handle, description]
            JSAtom atom = JS_ValueToAtom(_ctx, value);
            const char *description = JS_AtomToCString(_ctx, atom);
            writer->array(3);
            writer->integer(VALUE_SYMBOL);
            writer->integer(addHandle(value, group));
            writer->text(orEmpty(description));
            JS_FreeCString(_ctx, description);
            JS_FreeAtom(_ctx, atom);
            return;
        }
        case JS_TAG_OBJECT: {
            writeObject(writer, value, group);
            return;
        }
        default: {
            break;
        }
    }

    // the tags not listed: a float is written inline
    if (JS_IsNumber(value)) {
        double number = 0;
        JS_ToFloat64(_ctx, &number, value);
        writer->floating(number);
        return;
    }

    // a BigInt as [VALUE_BIGINT, decimal text]
    if (JS_IsBigInt(_ctx, value)) {
        const char *text = JS_ToCString(_ctx, value);
        writer->array(2);
        writer->integer(VALUE_BIGINT);
        writer->text(orEmpty(text));
        JS_FreeCString(_ctx, text);
        return;
    }

    // anything else: internal values (catch offsets...), which never reach JS
    writer->undefined();
}

/**
 * Name shown for the class of an object: the name of its constructor, else
 * the class name of the engine. malloc'd.
 */
char *Debugger::objectClassName(JSValueConst object, const JSDebugObjectInfo *info) {
    // the own "name" of the own "constructor" of the prototype, data
    // properties only (no getter runs); not for a proxy: it would run its
    // getPrototypeOf trap
    char *name = nullptr;
    if (info->kind != JS_DEBUG_OBJECT_PROXY) {
        JSValue prototype = JS_GetPrototype(_ctx, object);
        if (JS_IsException(prototype)) {
            JS_FreeValue(_ctx, JS_GetException(_ctx));
        } else if (JS_IsObject(prototype)) {
            JSValue constructor = ownDataProperty(_ctx, prototype, "constructor");
            if (JS_IsFunction(_ctx, constructor)) {
                name = ownStringProperty(_ctx, constructor, "name");
            }

            JS_FreeValue(_ctx, constructor);
        }

        JS_FreeValue(_ctx, prototype);
    }

    // no name (or an empty one): the class name of the engine
    if (name == nullptr || name[0] == 0) {
        memory::release(name);
        const char *className = JS_AtomToCString(_ctx, info->class_name);
        name = copyString(orEmpty(className));
        JS_FreeCString(_ctx, className);
    }

    return name;
}

/**
 * The "length" of an Array (a data property: no getter runs), else -1.
 */
int64_t Debugger::arrayLength(JSValueConst object) {
    JSValue length = ownDataProperty(_ctx, object, "length");
    int64_t result = -1;
    if (JS_IsNumber(length)) {
        JS_ToInt64(_ctx, &result, length);
    }

    JS_FreeValue(_ctx, length);
    return result;
}

/**
 * Short description, like Chrome's: "Array(3)", "Map(2)", "Error: text\n at...".
 * malloc'd.
 */
char *Debugger::describeObject(JSValueConst object, const JSDebugObjectInfo *info, const char *className) {
    switch (info->kind) {
        case JS_DEBUG_OBJECT_FUNCTION: {
            // "function name()"
            char *name = ownStringProperty(_ctx, object, "name");
            char *description = formatFunction(orEmpty(name));
            memory::release(name);
            return description;
        }
        case JS_DEBUG_OBJECT_ARRAY: {
            // "Class(length)"
            return formatWithCount(className, arrayLength(object));
        }
        case JS_DEBUG_OBJECT_MAP:
        case JS_DEBUG_OBJECT_SET:
        case JS_DEBUG_OBJECT_TYPED_ARRAY:
        case JS_DEBUG_OBJECT_ARRAY_BUFFER: {
            // "Class(size)"
            return formatWithCount(className, info->size);
        }
        case JS_DEBUG_OBJECT_ERROR: {
            // "Class: message\nstack"
            char *message = ownStringProperty(_ctx, object, "message");
            char *stack = ownStringProperty(_ctx, object, "stack");
            size_t size = strlen(className) + strlen(orEmpty(message)) + strlen(orEmpty(stack)) + 8;
            char *description = (char *)memory::allocate(size);
            snprintf(description, size, "%s: %s\n%s", className, orEmpty(message), orEmpty(stack));
            memory::release(message);
            memory::release(stack);
            return description;
        }
        case JS_DEBUG_OBJECT_DATE: {
            // ISO 8601, from the time value
            JSValue timeValue = JS_DebugGetInternal(_ctx, object, JS_DEBUG_INTERNAL_PRIMITIVE_VALUE);
            double time = 0;
            JS_ToFloat64(_ctx, &time, timeValue);
            JS_FreeValue(_ctx, timeValue);
            return formatDate(time);
        }
        case JS_DEBUG_OBJECT_REGEXP: {
            // source and flags, from the accessors of RegExp.prototype (the
            // script may have replaced them: writeObject() guards this call)
            JSValue source = JS_GetPropertyStr(_ctx, object, "source");
            JSValue flags = JS_GetPropertyStr(_ctx, object, "flags");
            const char *sourceText = JS_IsString(source) ? JS_ToCString(_ctx, source) : nullptr;
            const char *flagsText = JS_IsString(flags) ? JS_ToCString(_ctx, flags) : nullptr;

            // "/source/flags"
            size_t size = strlen(orEmpty(sourceText)) + strlen(orEmpty(flagsText)) + 4;
            char *description = (char *)memory::allocate(size);
            snprintf(description, size, "/%s/%s", orEmpty(sourceText), orEmpty(flagsText));

            // free the texts and values; an exception thrown by an accessor is
            // dropped
            JS_FreeCString(_ctx, sourceText);
            JS_FreeCString(_ctx, flagsText);
            JS_FreeValue(_ctx, source);
            JS_FreeValue(_ctx, flags);
            if (JS_HasException(_ctx)) {
                JS_FreeValue(_ctx, JS_GetException(_ctx));
            }

            return description;
        }
        default: {
            // the class name
            return copyString(className);
        }
    }
}

/**
 * Writes [VALUE_OBJECT, handle, kind, class name, description, size,
 * location].
 */
void Debugger::writeObject(cbor::Writer *writer, JSValueConst object, int group) {
    // kind and size from the engine
    JSDebugObjectInfo info;
    JS_DebugGetObjectInfo(_ctx, object, &info);

    // class name and description (both malloc'd); they may run script code
    // (RegExp accessors replaced by the script, proxy prototype): with
    // _requestDepth raised, a stop in that code does not open a pause
    // inside the request (or inside the pause) being answered
    _requestDepth++;
    char *className = objectClassName(object, &info);
    char *description = describeObject(object, &info, className);
    _requestDepth--;

    // the size of an array is its "length"
    int64_t size = info.kind == JS_DEBUG_OBJECT_ARRAY ? arrayLength(object) : info.size;

    // the fields up to the size; the object gets a handle in 'group'
    writer->array(7);
    writer->integer(VALUE_OBJECT);
    writer->integer(addHandle(object, group));
    writer->integer(info.kind);
    writer->text(className);
    writeText(writer, description, strlen(description));
    writer->integer(size);

    // location: [module, function number] for a bytecode function, else null
    JSAtom module = JS_ATOM_NULL;
    int number = 0;
    if (JS_DebugGetFunctionLocation(_ctx, object, &module, &number) == 0) {
        const char *moduleName = JS_AtomToCString(_ctx, module);
        writer->array(2);
        writer->text(orEmpty(moduleName));
        writer->integer(number);
        JS_FreeCString(_ctx, moduleName);
    } else {
        writer->null();
    }

    memory::release(className);
    memory::release(description);
}

// ### Properties ##############################################################

/**
 * Writes [name, flags, value, getter, setter] for each own property (at most
 * MAX_PROPERTIES) and adds their number to *propertyCount.
 */
void Debugger::writeProperties(cbor::Writer *writer, JSValueConst object, int group, int *propertyCount) {
    // a proxy has none here (its ownKeys trap would run): see its internals
    JSDebugObjectInfo info;
    if (JS_DebugGetObjectInfo(_ctx, object, &info) < 0 || info.kind == JS_DEBUG_OBJECT_PROXY) {
        return;
    }

    // the own string and symbol keys
    JSPropertyEnum *names = nullptr;
    uint32_t nameCount = 0;
    int flags = JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK;
    if (JS_GetOwnPropertyNames(_ctx, &names, &nameCount, object, flags) < 0) {
        JS_FreeValue(_ctx, JS_GetException(_ctx));
        return;
    }

    // for each key, up to MAX_PROPERTIES
    for (uint32_t i = 0; i < nameCount && *propertyCount < MAX_PROPERTIES; i++) {
        // its descriptor; a key that vanished or fails is skipped
        JSPropertyDescriptor descriptor;
        int found = JS_GetOwnProperty(_ctx, &descriptor, object, names[i].atom);
        if (found <= 0) {
            if (found < 0) {
                JS_FreeValue(_ctx, JS_GetException(_ctx));
            }

            continue;
        }

        // its flags as protocol PROPERTY_* bits
        bool isAccessor = (descriptor.flags & JS_PROP_GETSET) != 0;
        int propertyFlags = 0;
        if (descriptor.flags & JS_PROP_WRITABLE) {
            propertyFlags |= PROPERTY_WRITABLE;
        }

        if (descriptor.flags & JS_PROP_ENUMERABLE) {
            propertyFlags |= PROPERTY_ENUMERABLE;
        }

        if (descriptor.flags & JS_PROP_CONFIGURABLE) {
            propertyFlags |= PROPERTY_CONFIGURABLE;
        }

        if (isAccessor) {
            propertyFlags |= PROPERTY_ACCESSOR;
        }

        // the entry: a data property has its value and no getter / setter, an
        // accessor the opposite (the getter is never called)
        writer->array(5);
        JSValue name = JS_AtomToValue(_ctx, names[i].atom);
        writeValue(writer, name, group);   // a string, or a symbol
        JS_FreeValue(_ctx, name);
        writer->integer(propertyFlags);
        if (isAccessor) {
            writer->undefined();
            writeValue(writer, descriptor.getter, group);
            writeValue(writer, descriptor.setter, group);
        } else {
            writeValue(writer, descriptor.value, group);
            writer->undefined();
            writer->undefined();
        }

        // free the descriptor, count the property
        JS_FreeValue(_ctx, descriptor.value);
        JS_FreeValue(_ctx, descriptor.getter);
        JS_FreeValue(_ctx, descriptor.setter);
        (*propertyCount)++;
    }

    JS_FreePropertyEnum(_ctx, names, nameCount);
}

/**
 * Writes [name, value] for each internal property: [[Prototype]], [[Target]]
 * / [[Handler]] of a proxy, [[PrimitiveValue]], [[TargetFunction]],
 * [[PromiseState]] / [[PromiseResult]]. Sets *internalCount.
 */
void Debugger::writeInternals(cbor::Writer *writer, JSValueConst object, int group, int *internalCount) {
    // the kind of the object
    JSDebugObjectInfo info;
    if (JS_DebugGetObjectInfo(_ctx, object, &info) < 0) {
        return;
    }

    // collect (name, new reference) pairs; a proxy gives its target and
    // handler instead of its prototype (getPrototypeOf would run its trap)
    struct Internal {
        const char *name;   // "[[Prototype]]"..., static string
        JSValue value;      // owned (new reference)
    };
    Internal internals[6];   // at most 4 are used
    int count = 0;
    if (info.kind == JS_DEBUG_OBJECT_PROXY) {
        internals[count++] = { "[[Target]]", JS_DebugGetInternal(_ctx, object, JS_DEBUG_INTERNAL_PROXY_TARGET) };
        internals[count++] = { "[[Handler]]", JS_DebugGetInternal(_ctx, object, JS_DEBUG_INTERNAL_PROXY_HANDLER) };
    } else {
        JSValue prototype = JS_GetPrototype(_ctx, object);
        if (JS_IsException(prototype)) {
            JS_FreeValue(_ctx, JS_GetException(_ctx));
            prototype = JS_NULL;
        }

        internals[count++] = { "[[Prototype]]", prototype };
    }

    // the primitive value of a wrapper (new Number(1)...)
    if (info.kind == JS_DEBUG_OBJECT_WRAPPER) {
        internals[count++] = { "[[PrimitiveValue]]", JS_DebugGetInternal(_ctx, object, JS_DEBUG_INTERNAL_PRIMITIVE_VALUE) };
    }

    // the target of a bound function
    JSValue boundTarget = JS_DebugGetInternal(_ctx, object, JS_DEBUG_INTERNAL_BOUND_TARGET);
    if (!JS_IsUndefined(boundTarget)) {
        internals[count++] = { "[[TargetFunction]]", boundTarget };
    }

    // the state of a promise, and its result once settled
    if (info.kind == JS_DEBUG_OBJECT_PROMISE) {
        JSPromiseStateEnum state = JS_PromiseState(_ctx, object);
        const char *stateName = state == JS_PROMISE_PENDING ? "pending"
                              : state == JS_PROMISE_FULFILLED ? "fulfilled" : "rejected";
        internals[count++] = { "[[PromiseState]]", JS_NewString(_ctx, stateName) };
        if (state != JS_PROMISE_PENDING) {
            internals[count++] = { "[[PromiseResult]]", JS_PromiseResult(_ctx, object) };
        }
    }

    // write each pair and free its value
    for (int i = 0; i < count; i++) {
        writer->array(2);
        writer->text(internals[i].name);
        writeValue(writer, internals[i].value, group);
        JS_FreeValue(_ctx, internals[i].value);
    }

    *internalCount = count;
}

// ### Reading values sent by the host #########################################

/**
 * Reads a value of the VALUE encoding into *value (new reference). False if
 * it is invalid or names an unknown handle.
 */
bool Debugger::readValue(cbor::Reader *reader, JSValue *value) {
    // primitives (undefined, null, booleans, integers, floats, text) give the
    // matching JS value; an array is a tagged value (below); anything else
    // is invalid
    switch (reader->peek()) {
        case cbor::Type::Undefined: {
            *value = JS_UNDEFINED;
            return reader->undefined();
        }
        case cbor::Type::Null: {
            *value = JS_NULL;
            return reader->null();
        }
        case cbor::Type::True:
        case cbor::Type::False: {
            bool flag = false;
            bool ok = reader->boolean(&flag);
            *value = JS_NewBool(_ctx, flag);
            return ok;
        }
        case cbor::Type::Unsigned:
        case cbor::Type::Negative: {
            int64_t number = 0;
            bool ok = reader->integer(&number);
            *value = JS_NewInt64(_ctx, number);
            return ok;
        }
        case cbor::Type::Float: {
            double number = 0;
            bool ok = reader->floating(&number);
            *value = JS_NewFloat64(_ctx, number);
            return ok;
        }
        case cbor::Type::Text: {
            const char *text = nullptr;
            size_t length = 0;
            bool ok = reader->text(&text, &length);
            *value = ok ? JS_NewStringLen(_ctx, text, length) : JS_UNDEFINED;
            return ok;
        }
        case cbor::Type::Array: {
            break;
        }
        default: {
            return false;
        }
    }

    // a tagged value: [tag, ...]
    uint64_t count = 0;
    int64_t tag = 0;
    if (!reader->array(&count) || count < 1 || !reader->integer(&tag)) {
        return false;
    }

    // [VALUE_HANDLE, handle]: the kept object
    if (tag == VALUE_HANDLE && count == 2) {
        int64_t handle = 0;
        return reader->integer(&handle) && findHandle(handle, value);
    }

    // [VALUE_BIGINT, decimal text]: a BigInt (64-bit range only)
    if (tag == VALUE_BIGINT && count == 2) {
        const char *text = nullptr;
        size_t length = 0;
        if (!reader->text(&text, &length) || length >= 32) {
            return false;
        }

        // a zero-terminated copy for strtoll
        char digits[32];
        memcpy(digits, text, length);
        digits[length] = 0;

        // the whole text must be a number of the 64-bit range: strtoll
        // saturates out of range and only says so through errno
        char *end = nullptr;
        errno = 0;
        long long number = strtoll(digits, &end, 10);
        bool isWholeText = end != digits && *end == 0;
        bool inRange = errno != ERANGE;
        if (!isWholeText || !inRange) {
            return false;
        }

        // created only once valid: nothing to free on failure
        *value = JS_NewBigInt64(_ctx, number);
        return true;
    }

    // [VALUE_UNINITIALIZED]: undefined
    if (tag == VALUE_UNINITIALIZED && count == 1) {
        *value = JS_UNDEFINED;
        return true;
    }

    // any other tag
    return false;
}

} // namespace agent
