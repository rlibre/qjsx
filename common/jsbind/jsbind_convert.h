// jsbind - conversions between JS values and C++ types.
//
// Convert<T>::from(ctx, value, out, where) converts a JS value into 'out'; on
// error it records a JS exception that says where the value came from
// ("UART.open: argument 1, field 'speed': must be an integer") and returns false.
// Convert<T>::to(ctx, value) returns a new JS value (or JS_EXCEPTION).
//
// Enums and structs are described with JS_ENUM / JS_STRUCT (below); classes
// with JS_CLASS (jsbind_class.h).
#pragma once

#include <math.h>
#include "jsbind_value.h"

namespace js {
namespace detail {

// Where a value comes from, for error messages.
struct Where {
    const char *function;   // "UART.open"
    int         argument;           // 1-based, 0 if not an argument
    const char *field;      // struct field name, or nullptr
};

/**
 * Writes the place of a value into 'out': "UART.open: argument 1, field
 * 'speed'" (each part only when known; truncated to 'size').
 */
inline void describe(char *out, size_t size, const Where &where) {
    // the function name (or "value"), then the argument number if known and
    // if there is room left
    int length = snprintf(out, size, "%s", where.function != nullptr ? where.function : "value");
    if (where.argument > 0 && length >= 0 && (size_t)length < size) {
        length += snprintf(out + length, size - length, ": argument %d", where.argument);
    }

    // then the field name, same conditions
    if (where.field != nullptr && length >= 0 && (size_t)length < size) {
        snprintf(out + length, size - length, ", field '%s'", where.field);
    }
}

/**
 * Records "<place>: <expectation>" (TypeError by default) in the current call.
 * Always false, so that a 'from' function can 'return conversionError(...)'.
 */
template<class Kind = TypeError>
inline bool conversionError(const Where &where, const char *expectation) {
    char place[256];
    describe(place, sizeof(place), where);
    fail<Kind>("%s: %s", place, expectation);
    return false;
}

/**
 * Clears an exception thrown by a probing QuickJS call.
 */
inline void clearException(JSContext *ctx) {
    JS_FreeValue(ctx, JS_GetException(ctx));
}

/**
 * View on the bytes of a typed array or an ArrayBuffer; false (no exception
 * pending) if neither. '*buffer': new reference on the ArrayBuffer to free.
 */
inline bool bytesOf(JSContext *ctx, JSValueConst value, uint8_t **data, size_t *size, JSValue *buffer) {
    // first try a typed array: its bytes are a slice of its ArrayBuffer
    size_t offset = 0;
    size_t length = 0;
    size_t elementSize = 0;
    JSValue arrayBuffer = JS_GetTypedArrayBuffer(ctx, value, &offset, &length, &elementSize);
    if (!JS_IsException(arrayBuffer)) {
        // memory of the ArrayBuffer (nullptr if detached)
        size_t bufferSize = 0;
        uint8_t *base = JS_GetArrayBuffer(ctx, &bufferSize, arrayBuffer);
        if (base == nullptr) {
            JS_FreeValue(ctx, arrayBuffer);
            clearException(ctx);
            return false;
        }

        // the slice; the caller gets our reference on the ArrayBuffer
        *data = base + offset;
        *size = length;
        *buffer = arrayBuffer;
        return true;
    }

    // not a typed array (forget that error): try an ArrayBuffer itself
    clearException(ctx);
    size_t bufferSize = 0;
    uint8_t *base = JS_GetArrayBuffer(ctx, &bufferSize, value);
    if (base == nullptr) {
        clearException(ctx);
        return false;
    }

    // all its bytes, with a new reference on it
    *data = base;
    *size = bufferSize;
    *buffer = JS_DupValue(ctx, value);
    return true;
}

/**
 * New Uint8Array holding a copy of 'size' bytes at 'data' (JS_EXCEPTION on
 * failure, with the exception pending).
 */
inline JSValue newUint8Array(JSContext *ctx, const void *data, size_t size) {
    // copy the bytes into a new ArrayBuffer
    JSValue arrayBuffer = JS_NewArrayBufferCopy(ctx, (const uint8_t *)data, size);
    if (JS_IsException(arrayBuffer)) {
        return arrayBuffer;
    }

    // wrap it: new Uint8Array(buffer, 0, size); JS_NewTypedArray reads the
    // offset and the length even when they are not passed, so they are given
    JSValue arguments[3] = {
        arrayBuffer,
        JS_NewInt32(ctx, 0),
        JS_NewInt64(ctx, (int64_t)size),
    };
    JSValue array = JS_NewTypedArray(ctx, 3, arguments, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, arrayBuffer);
    return array;
}

/**
 * True if the integer type T is signed.
 */
template<class T> constexpr bool isSigned() {
    return T(-1) < T(0);
}

/**
 * 2^exponent as a double (exact: a power of two, up to 2^64 here).
 */
constexpr double powerOfTwo(int exponent) {
    double result = 1.0;

    for (int i = 0; i < exponent; i++) {
        result *= 2.0;
    }

    return result;
}

/**
 * Smallest value of the integer type T, as a double (exact: -2^(bits-1) or 0).
 */
template<class T> constexpr double lowestOf() {
    return isSigned<T>() ? -powerOfTwo(sizeof(T) * 8 - 1) : 0.0;
}

/**
 * First integer past the largest value of T, as a double: 2^(bits-1) or
 * 2^bits. Exact, unlike the largest value itself for the 64-bit types
 * (2^63 - 1 rounds up to 2^63 as a double), so the range test is
 * 'number < limitOf<T>()'.
 */
template<class T> constexpr double limitOf() {
    return isSigned<T>() ? powerOfTwo(sizeof(T) * 8 - 1) : powerOfTwo(sizeof(T) * 8);
}

} // namespace detail

template<class T, class Enable = void>
struct Convert;   // no generic conversion: unsupported types fail to compile here

// ### Numbers and booleans ####################################################

template<>
struct Convert<bool> {
    /**
     * Accepts only true and false (no truthiness: 0 or "" are rejected).
     */
    static bool from(JSContext *, JSValueConst value, bool &out, const detail::Where &where) {
        if (!JS_IsBool(value)) {
            return detail::conversionError(where, "must be a boolean");
        }

        out = JS_VALUE_GET_BOOL(value) != 0;
        return true;
    }

    /**
     * JS boolean.
     */
    static JSValue to(JSContext *ctx, bool value) {
        return JS_NewBool(ctx, value);
    }
};

/**
 * Conversion of the integer types (Convert<int>, Convert<uint8_t>...).
 */
template<class T>
struct IntegerConvert {
    /**
     * Accepts a JS number with no fractional part (TypeError otherwise, strings
     * included) that fits in T (RangeError otherwise).
     */
    static bool from(JSContext *ctx, JSValueConst value, T &out, const detail::Where &where) {
        // a number: no implicit conversion from strings
        double number = 0;
        if (!JS_IsNumber(value) || JS_ToFloat64(ctx, &number, value) < 0) {
            return detail::conversionError(where, "must be an integer");
        }

        // with no fractional part
        if (number != floor(number)) {
            return detail::conversionError(where, "must be an integer");
        }

        // within the range of T (both bounds exact doubles: the cast below is
        // defined for every number that passes)
        if (number < detail::lowestOf<T>() || number >= detail::limitOf<T>()) {
            return detail::conversionError<RangeError>(where, "integer out of range");
        }

        out = (T)number;
        return true;
    }

    /**
     * JS number.
     */
    static JSValue to(JSContext *ctx, T value) {
        // signed types: through int64
        if (detail::isSigned<T>()) {
            return JS_NewInt64(ctx, (int64_t)value);
        }

        // unsigned types: through double, so that large 64-bit values stay
        // positive
        return JS_NewFloat64(ctx, (double)value);
    }
};

template<> struct Convert<signed char> : IntegerConvert<signed char> {};
template<> struct Convert<unsigned char> : IntegerConvert<unsigned char> {};
template<> struct Convert<short> : IntegerConvert<short> {};
template<> struct Convert<unsigned short> : IntegerConvert<unsigned short> {};
template<> struct Convert<int> : IntegerConvert<int> {};
template<> struct Convert<unsigned int> : IntegerConvert<unsigned int> {};
template<> struct Convert<long> : IntegerConvert<long> {};
template<> struct Convert<unsigned long> : IntegerConvert<unsigned long> {};
template<> struct Convert<long long> : IntegerConvert<long long> {};
template<> struct Convert<unsigned long long> : IntegerConvert<unsigned long long> {};

/**
 * Conversion of float and double.
 */
template<class T>
struct FloatConvert {
    /**
     * Accepts any JS number (NaN and infinities included).
     */
    static bool from(JSContext *ctx, JSValueConst value, T &out, const detail::Where &where) {
        double number = 0;
        if (!JS_IsNumber(value) || JS_ToFloat64(ctx, &number, value) < 0) {
            return detail::conversionError(where, "must be a number");
        }

        out = (T)number;
        return true;
    }

    /**
     * JS number.
     */
    static JSValue to(JSContext *ctx, T value) {
        return JS_NewFloat64(ctx, (double)value);
    }
};

template<> struct Convert<double> : FloatConvert<double> {};
template<> struct Convert<float> : FloatConvert<float> {};

template<class T, long long Lo, long long Hi>
struct Convert<Range<T, Lo, Hi>> {
    /**
     * Accepts what T accepts (same errors), within [Lo, Hi] (RangeError
     * otherwise).
     */
    static bool from(JSContext *ctx, JSValueConst value, Range<T, Lo, Hi> &out, const detail::Where &where) {
        // convert as a T first
        T number;
        if (!Convert<T>::from(ctx, value, number, where)) {
            return false;
        }

        // then check the bounds
        if (number < (T)Lo || number > (T)Hi) {
            char expectation[96];
            snprintf(expectation, sizeof(expectation), "must be between %lld and %lld", Lo, Hi);
            return detail::conversionError<RangeError>(where, expectation);
        }

        out.value = number;
        return true;
    }

    /**
     * Same as the conversion of T.
     */
    static JSValue to(JSContext *ctx, const Range<T, Lo, Hi> &value) {
        return Convert<T>::to(ctx, value.value);
    }
};

// ### Strings and bytes #######################################################

template<>
struct Convert<Str> {
    /**
     * Accepts only strings (no implicit toString). 'out' owns the UTF-8 text
     * and frees it when destroyed.
     */
    static bool from(JSContext *ctx, JSValueConst value, Str &out, const detail::Where &where) {
        // a string: no implicit toString
        if (!JS_IsString(value)) {
            return detail::conversionError(where, "must be a string");
        }

        // get its UTF-8 text (fails only with an exception pending)
        size_t size = 0;
        const char *text = JS_ToCStringLen(ctx, &size, value);
        if (text == nullptr) {
            detail::markPendingException();
            return false;
        }

        // 'out' takes it
        out = Str::fromJS(ctx, text, size);
        return true;
    }

    /**
     * New JS string from the UTF-8 bytes.
     */
    static JSValue to(JSContext *ctx, const Str &value) {
        return JS_NewStringLen(ctx, value.data(), value.size());
    }
};

template<>
struct Convert<const char *> {
    /**
     * New JS string from a C string; nullptr gives null. (No 'from': use
     * js::Str for strings received from JS.)
     */
    static JSValue to(JSContext *ctx, const char *value) {
        return value != nullptr ? JS_NewString(ctx, value) : JS_NULL;
    }
};

template<>
struct Convert<Bytes> {
    /**
     * Accepts a typed array or an ArrayBuffer: 'out' views its memory (no
     * copy) and holds the ArrayBuffer.
     */
    static bool from(JSContext *ctx, JSValueConst value, Bytes &out, const detail::Where &where) {
        // find the bytes and their ArrayBuffer
        uint8_t *data = nullptr;
        size_t size = 0;
        JSValue buffer = JS_UNDEFINED;
        if (!detail::bytesOf(ctx, value, &data, &size, &buffer)) {
            return detail::conversionError(where, "must be a Uint8Array, a typed array or an ArrayBuffer");
        }

        // the view takes the reference on the ArrayBuffer: it stays alive
        out = Bytes(data, size, Value(ctx, buffer));
        return true;
    }

    /**
     * New Uint8Array holding a copy of the bytes.
     */
    static JSValue to(JSContext *ctx, const Bytes &value) {
        return detail::newUint8Array(ctx, value.data(), value.size());
    }
};

template<>
struct Convert<Buffer> {
    /**
     * Accepts a typed array or an ArrayBuffer: 'out' keeps the value received
     * (not its ArrayBuffer) and views its memory.
     */
    static bool from(JSContext *ctx, JSValueConst value, Buffer &out, const detail::Where &where) {
        // find the bytes
        uint8_t *data = nullptr;
        size_t size = 0;
        JSValue buffer = JS_UNDEFINED;
        if (!detail::bytesOf(ctx, value, &data, &size, &buffer)) {
            return detail::conversionError(where, "must be a Uint8Array, a typed array or an ArrayBuffer");
        }

        // keep the value received rather than its ArrayBuffer: to() gives
        // the same array back
        JS_FreeValue(ctx, buffer);
        out = Buffer(Value::borrow(ctx, value), data, size);
        return true;
    }

    /**
     * The same JS array (no copy): JS and C++ share the bytes.
     */
    static JSValue to(JSContext *, const Buffer &value) {
        return value.value().dup();
    }
};

/**
 * New Uint8Array holding a copy of the bytes. On failure: an empty Buffer, the
 * error recorded in the current call, or printed outside of any call (also
 * when there is no context at all). Nothing is created once the current call
 * has failed.
 */
inline Buffer Buffer::copy(const void *data, size_t size, JSContext *ctx) {
    // the given context, else the current call's; none: nowhere to create it
    JSContext *target = ctx != nullptr ? ctx : js::context();
    if (target == nullptr) {
        fail("Buffer.copy: no context (outside of a JS call, pass one)");
        return Buffer();
    }

    // the first error wins: once the current call failed, nothing is created
    if (failed()) {
        return Buffer();
    }

    // create the copy
    JSValue array = detail::newUint8Array(target, data, size);
    if (JS_IsException(array)) {
        detail::propagateException(target);
        return Buffer();
    }

    // find where its bytes live; the Buffer keeps the array itself
    uint8_t *bytes = nullptr;
    size_t length = 0;
    JSValue arrayBuffer = JS_UNDEFINED;
    detail::bytesOf(target, array, &bytes, &length, &arrayBuffer);
    JS_FreeValue(target, arrayBuffer);
    return Buffer(Value(target, array), bytes, length);
}

/**
 * New zero-filled Uint8Array. On failure: an empty Buffer, the error recorded
 * in the current call, or printed outside of any call (also when there is no
 * context at all). Nothing is created once the current call has failed.
 */
inline Buffer Buffer::create(size_t size, JSContext *ctx) {
    // the given context, else the current call's; none: nowhere to create it
    JSContext *target = ctx != nullptr ? ctx : js::context();
    if (target == nullptr) {
        fail("Buffer.create: no context (outside of a JS call, pass one)");
        return Buffer();
    }

    // the first error wins: once the current call failed, nothing is created
    if (failed()) {
        return Buffer();
    }

    // new Uint8Array(size), zero-filled (JS_NewTypedArray always reads 3
    // arguments)
    JSValue arguments[3] = { JS_NewInt64(target, (int64_t)size), JS_UNDEFINED, JS_UNDEFINED };
    JSValue created = JS_NewTypedArray(target, 3, arguments, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(target, arguments[0]);
    if (JS_IsException(created)) {
        detail::propagateException(target);
        return Buffer();
    }

    // find where its bytes live; the Buffer keeps the array itself
    uint8_t *bytes = nullptr;
    size_t byteCount = 0;
    JSValue arrayBuffer = JS_UNDEFINED;
    detail::bytesOf(target, created, &bytes, &byteCount, &arrayBuffer);
    JS_FreeValue(target, arrayBuffer);
    return Buffer(Value(target, created), bytes, byteCount);
}

// ### Values and functions ####################################################

template<>
struct Convert<Value> {
    /**
     * Accepts anything: 'out' takes a new reference on the value.
     */
    static bool from(JSContext *ctx, JSValueConst value, Value &out, const detail::Where &) {
        out = Value::borrow(ctx, value);
        return true;
    }

    /**
     * New reference on the value.
     */
    static JSValue to(JSContext *, const Value &value) {
        return value.dup();
    }
};

template<>
struct Convert<Function> {
    /**
     * Accepts only functions: 'out' takes a new reference on it.
     */
    static bool from(JSContext *ctx, JSValueConst value, Function &out, const detail::Where &where) {
        if (!JS_IsFunction(ctx, value)) {
            return detail::conversionError(where, "must be a function");
        }

        out = Function(ctx, JS_DupValue(ctx, value));
        return true;
    }

    /**
     * New reference on the function.
     */
    static JSValue to(JSContext *, const Function &value) {
        return value.dup();
    }
};

template<class T>
struct Convert<Optional<T>> {
    /**
     * undefined (or a missing argument / field) gives an absent value;
     * anything else must convert as a T. null is not "absent".
     */
    static bool from(JSContext *ctx, JSValueConst value, Optional<T> &out, const detail::Where &where) {
        // undefined: absent
        if (JS_IsUndefined(value)) {
            out = Optional<T>();
            return true;
        }

        // anything else (null included) must convert as a T
        T converted;
        if (!Convert<T>::from(ctx, value, converted, where)) {
            return false;
        }

        out.set(meta::move(converted));
        return true;
    }

    /**
     * undefined when absent, else the conversion of T.
     */
    static JSValue to(JSContext *ctx, const Optional<T> &value) {
        return value.has() ? Convert<T>::to(ctx, value.value()) : JS_UNDEFINED;
    }
};

// ### Calling JS functions from C++ ###########################################

/**
 * Calls the function with this 'this' and the C++ arguments. Undefined when
 * nothing ran or when it threw (see the comment of class Function).
 */
template<class... A>
Value Function::callWith(JSValueConst thisValue, const A &...arguments) const {
    // unset function: nothing to call
    JSContext *ctx = context();
    if (ctx == nullptr || isUndefined()) {
        return Value();
    }

    // no JS may run during a finalizer
    if (detail::finalizing > 0) {
        fprintf(stderr, "jsbind: a JS function cannot be called from a finalizer\n");
        return Value();
    }

    // the first error wins: once the current call failed, no more JS runs
    if (js::failed()) {
        return Value();
    }

    // convert the arguments with Convert<A>::to (one spare slot: a C++ array
    // cannot be empty)
    constexpr int count = sizeof...(A);
    JSValue argv[count + 1] = { Convert<meta::Decay<A>>::to(ctx, arguments)..., JS_UNDEFINED };
    bool converted = true;

    for (int i = 0; i < count; i++) {
        if (JS_IsException(argv[i])) {
            converted = false;
        }
    }

    // call, unless an argument failed to convert (its exception is pending)
    JSValue result = JS_EXCEPTION;
    if (converted) {
        result = JS_Call(ctx, get(), thisValue, count, argv);
    }

    // free the converted arguments
    for (int i = 0; i < count; i++) {
        JS_FreeValue(ctx, argv[i]);
    }

    // a conversion failed or the function threw: inside a call from JS, the
    // exception propagates to JS when the C++ function returns; outside of
    // any call (event loop), it is printed as uncaught
    if (JS_IsException(result)) {
        detail::propagateException(ctx);
        return Value();
    }

    return Value(ctx, result);
}

/**
 * Calls the function with 'this' undefined (see callWith).
 */
template<class... A>
Value Function::operator()(const A &...arguments) const {
    return callWith(JS_UNDEFINED, arguments...);
}

// #############################################################################
// Enums: JS_ENUM(Parity, none, odd, even) declares 'enum class Parity' whose
// values are passed from JS as the strings "none", "odd", "even".

namespace detail {

struct EnumNames {
    const char *const *names;  // JS strings, indexed by enum value
    int count;                 // number of values
};

template<class E, class = void> struct IsEnum { static constexpr bool value = false; };
template<class E>
struct IsEnum<E, meta::Void<decltype(jsEnumNames((E *)nullptr))>> {
    static constexpr bool value = true;
};

} // namespace detail

template<class E>
struct Convert<E, typename meta::EnableIf<detail::IsEnum<E>::value>::Type> {
    /**
     * Accepts one of the names listed in JS_ENUM (the enum value is its index);
     * anything else is a TypeError that lists the accepted names.
     */
    static bool from(JSContext *ctx, JSValueConst value, E &out, const detail::Where &where) {
        // a string: look for it among the names
        detail::EnumNames names = jsEnumNames((E *)nullptr);
        if (JS_IsString(value)) {
            size_t size = 0;
            const char *text = JS_ToCStringLen(ctx, &size, value);

            for (int i = 0; text != nullptr && i < names.count; i++) {
                bool same = strlen(names.names[i]) == size && memcmp(names.names[i], text, size) == 0;
                if (same) {
                    JS_FreeCString(ctx, text);
                    out = (E)i;
                    return true;
                }
            }

            JS_FreeCString(ctx, text);
        }

        // not found: the error lists the accepted names ("must be one of
        // 'a', 'b'"), as many as fit
        char expectation[256];
        int length = snprintf(expectation, sizeof(expectation), "must be one of");

        for (int i = 0; i < names.count && length >= 0 && (size_t)length < sizeof(expectation); i++) {
            length += snprintf(expectation + length, sizeof(expectation) - length,
                               "%s '%s'", i == 0 ? "" : ",", names.names[i]);
        }

        return detail::conversionError(where, expectation);
    }

    /**
     * The name of the value; undefined for a value not listed in JS_ENUM.
     */
    static JSValue to(JSContext *ctx, E value) {
        detail::EnumNames names = jsEnumNames((E *)nullptr);
        int index = (int)value;
        return index >= 0 && index < names.count ? JS_NewString(ctx, names.names[index]) : JS_UNDEFINED;
    }
};

// #############################################################################
// Structs: JS_STRUCT(UartOptions, speed, parity) after the struct definition.
// From JS: a plain object; missing fields keep the C++ default values,
// unknown fields are rejected (catches typos). To JS: a new object.
//
// JS_STRUCT defines jsStructFields(object, visitor), which calls
// visitor.field(name, object, &Type::member) for each field; the visitors
// below give it its meaning (read, find a name, write).

namespace detail {

// Visitor: fills the struct from a JS object.
struct StructReader {
    JSContext   *ctx;     // context of the conversion
    JSValueConst object;  // the JS object read; borrowed
    const Where *where;   // argument being converted, for error messages
    bool         ok;      // false once a field failed

    /**
     * Converts the property 'name' into the field (undefined keeps the C++
     * default). Does nothing once a field failed.
     */
    template<class S, class F>
    void field(const char *name, S &target, F S::*member) {
        // an earlier field failed: skip the others
        if (!ok) {
            return;
        }

        // read the property (a getter may throw)
        JSValue property = JS_GetPropertyStr(ctx, object, name);
        if (JS_IsException(property)) {
            markPendingException();
            ok = false;
            return;
        }

        // convert it unless undefined; errors name the field
        if (!JS_IsUndefined(property)) {
            Where fieldWhere = { where->function, where->argument, name };
            ok = Convert<F>::from(ctx, property, target.*member, fieldWhere);
        }

        JS_FreeValue(ctx, property);
    }
};

// Visitor: tells whether a name is a field of the struct.
struct StructFieldFinder {
    const char *name;   // field name searched
    bool        found;  // true once it was found

    /**
     * Sets 'found' if this field is the one searched.
     */
    template<class S, class F>
    void field(const char *fieldName, S &, F S::*) {
        if (strcmp(fieldName, name) == 0) {
            found = true;
        }
    }
};

// Visitor: builds a JS object from the struct.
struct StructWriter {
    JSContext *ctx;     // context of the conversion
    JSValue    object;  // the JS object filled; borrowed (the caller owns it)

    /**
     * Converts the field and stores it as the property 'name'.
     */
    template<class S, class F>
    void field(const char *name, S &source, F S::*member) {
        JS_SetPropertyStr(ctx, object, name, Convert<F>::to(ctx, source.*member));
    }
};

template<class S, class = void> struct IsStruct { static constexpr bool value = false; };
template<class S>
struct IsStruct<S, meta::Void<decltype(jsStructFields(meta::declval<S &>(), meta::declval<StructFieldFinder &>()))>> {
    static constexpr bool value = true;
};

} // namespace detail

template<class S>
struct Convert<S, typename meta::EnableIf<detail::IsStruct<S>::value>::Type> {
    /**
     * Accepts an object whose own properties are all fields of S. 'out' keeps
     * its values for the missing fields: start from a default-constructed S.
     */
    static bool from(JSContext *ctx, JSValueConst value, S &out, const detail::Where &where) {
        // an object
        if (!JS_IsObject(value)) {
            return detail::conversionError(where, "must be an object");
        }

        // with no unknown property (catches typos)
        if (!rejectUnknownFields(ctx, value, where)) {
            return false;
        }

        // convert each field present into 'out'
        detail::StructReader reader = { ctx, value, &where, true };
        jsStructFields(out, reader);
        return reader.ok;
    }

    /**
     * New plain object with one property per field.
     */
    static JSValue to(JSContext *ctx, const S &value) {
        detail::StructWriter writer = { ctx, JS_NewObject(ctx) };
        jsStructFields((S &)value, writer);
        return writer.object;
    }

private:
    /**
     * True if every own enumerable string property of 'object' is a field of
     * S; otherwise records an error naming the first unknown one.
     */
    static bool rejectUnknownFields(JSContext *ctx, JSValueConst object, const detail::Where &where) {
        // list the own enumerable string properties
        JSPropertyEnum *properties = nullptr;
        uint32_t count = 0;
        int flags = JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY;
        if (JS_GetOwnPropertyNames(ctx, &properties, &count, object, flags) < 0) {
            detail::markPendingException();
            return false;
        }

        // check each name, up to the first unknown one
        bool ok = true;

        for (uint32_t i = 0; i < count && ok; i++) {
            // look the name up by visiting the fields of a scratch S; an
            // unknown name is an error that names it
            const char *name = JS_AtomToCString(ctx, properties[i].atom);
            S probe;
            detail::StructFieldFinder finder = { name != nullptr ? name : "", false };
            jsStructFields(probe, finder);
            if (!finder.found) {
                char expectation[160];
                snprintf(expectation, sizeof(expectation), "unknown field '%s'", finder.name);
                ok = detail::conversionError(where, expectation);
            }

            JS_FreeCString(ctx, name);
        }

        JS_FreePropertyEnum(ctx, properties, count);
        return ok;
    }
};

} // namespace js

/**
 * One field in jsStructFields (used by JS_STRUCT).
 */
#define JS_BIND_STRUCT_FIELD(Type, name) visitor.field(#name, object, &Type::name);

/**
 * Makes a struct convertible from / to a JS object: defines
 * jsStructFields(object, visitor), found by Convert<S> through argument
 * dependent lookup. Write it after the struct, in the same namespace.
 */
#define JS_STRUCT(Type, ...)                                                        \
    template<class Visitor>                                                         \
    inline void jsStructFields(Type &object, Visitor &visitor)                      \
    {                                                                               \
        JS_BIND_FOR_EACH(JS_BIND_STRUCT_FIELD, Type, __VA_ARGS__)                   \
    }

#define JS_BIND_ENUM_VALUE(unused, name) name,
#define JS_BIND_ENUM_NAME(unused, name) #name,

/**
 * Declares 'enum class Name' with the given values, and jsEnumNames(Name *)
 * which returns their names (in the same order) for Convert<Name>.
 */
#define JS_ENUM(Name, ...)                                                          \
    enum class Name { JS_BIND_FOR_EACH(JS_BIND_ENUM_VALUE, ~, __VA_ARGS__) };       \
    inline ::js::detail::EnumNames jsEnumNames(Name *)                              \
    {                                                                               \
        static const char *const names[] = { JS_BIND_FOR_EACH(JS_BIND_ENUM_NAME, ~, __VA_ARGS__) }; \
        return ::js::detail::EnumNames{ names, (int)(sizeof(names) / sizeof(names[0])) }; \
    }
