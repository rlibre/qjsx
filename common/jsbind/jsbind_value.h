/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// jsbind - value types used in C++ signatures.
//
//   js::Value     any JS value, reference counted
//   js::Function  a JS function that C++ can call
//   js::Str       a string received from JS (UTF-8 view) or given to JS
//   js::Bytes     bytes received from JS (view on an ArrayBuffer / typed array)
//                 or given to JS (copied into a new Uint8Array)
//   js::Buffer    a Uint8Array kept by C++ after the call
//   js::Optional  an argument or field that may be missing / undefined
//   js::Range     an integer or number constrained to [Lo, Hi]
//   js::Args      all the remaining arguments of a variadic function
//
// js::Value (and Function, Buffer) members of an object created by the
// binding register themselves while the object is being constructed, so that
// the garbage collector sees the values the object keeps (callbacks...).
#pragma once

#include "jsbind_error.h"
#include "memory.h"

namespace js {

class Value;

namespace detail {

// Header placed before every C++ object created by the binding.
struct InstanceHeader {
    JSRuntime      *rt;              // runtime of the JS object
    JSValue         self;            // the JS object; not counted (the object owns us)
    Value          *firstMember;     // js::Value members, reported to the GC
    bool            keptAlive;       // see js::keepAlive()
    bool            releasePending;  // keepAlive(false) done, reference not freed yet
    JSValue         keepReference;   // counted reference while kept alive
    InstanceHeader *nextKept;        // next in the 'kept' or 'released' list
    InstanceHeader *previousKept;    // previous in the 'kept' list; nullptr: first
};

// Memory range of the C++ object under construction, if any.
struct Constructing {
    const char     *begin;   // first byte of the object's storage
    const char     *end;     // one past its last byte
    InstanceHeader *header;  // header of the object being constructed
};

inline thread_local Constructing *constructing = nullptr;

} // namespace detail

/**
 * Owned reference on a JS value: the reference is freed by the destructor,
 * copies take a new reference, moves transfer it. A Value that is a member of
 * a bound C++ object is also linked into that object's member list (see
 * attach()), so the object's GC mark function reports it.
 */
class Value {
public:
    /**
     * Undefined value, with no context.
     */
    Value() : _ctx(nullptr), _rt(nullptr), _value(JS_UNDEFINED) {
        attach();
    }

    /**
     * Takes ownership of 'owned'.
     */
    Value(JSContext *ctx, JSValue owned)
        : _ctx(ctx), _rt(ctx != nullptr ? JS_GetRuntime(ctx) : nullptr), _value(owned) {
        attach();
    }

    /**
     * New reference on a value owned by someone else.
     */
    static Value borrow(JSContext *ctx, JSValueConst value) {
        return Value(ctx, JS_DupValue(ctx, value));
    }

    /**
     * Copy: takes a new reference on the same JS value.
     */
    Value(const Value &other) : _ctx(other._ctx), _rt(other._rt), _value(other.dup()) {
        attach();
    }

    /**
     * Move: takes the reference of 'other', which becomes undefined.
     * Membership in an object is not moved: it depends on the address.
     */
    Value(Value &&other) : _ctx(other._ctx), _rt(other._rt), _value(other._value) {
        other._value = JS_UNDEFINED;
        attach();
    }

    /**
     * Copy assignment: frees the current reference, takes a new one on the
     * value of 'other'. The member registration of this Value is unchanged.
     */
    Value &operator=(const Value &other) {
        // take the new reference before freeing the old one: both may be the
        // same JS value
        if (this != &other) {
            JSValue copy = other.dup();
            release();
            _ctx = other._ctx;
            _rt = other._rt;
            _value = copy;
        }

        return *this;
    }

    /**
     * Move assignment: frees the current reference, takes the one of
     * 'other', which becomes undefined.
     */
    Value &operator=(Value &&other) {
        if (this != &other) {
            release();
            _ctx = other._ctx;
            _rt = other._rt;
            _value = other._value;
            other._value = JS_UNDEFINED;
        }

        return *this;
    }

    /**
     * Frees the reference and unlinks the Value from its owning object.
     */
    ~Value() {
        release();
        detach();
    }

    /**
     * Borrowed value (do not free).
     */
    JSValueConst            get() const {
        return _value;
    }

    /**
     * New reference (to free or to give away).
     */
    JSValue                 dup() const {
        return _ctx != nullptr ? JS_DupValue(_ctx, _value) : JS_UNDEFINED;
    }

    /**
     * Context the value belongs to (nullptr for a default Value).
     */
    JSContext              *context() const {
        return _ctx;
    }

    /**
     * True if the value is undefined (default Value, moved-from, reset).
     */
    bool                    isUndefined() const {
        return JS_IsUndefined(_value);
    }

    /**
     * True if the value is null.
     */
    bool                    isNull() const {
        return JS_IsNull(_value);
    }

    /**
     * Frees the reference; the value becomes undefined.
     */
    void reset() {
        release();
        _value = JS_UNDEFINED;
    }

    /**
     * Used by the GC mark function of the owning object.
     */
    void                    mark(JSRuntime *rt, JS_MarkFunc *markFunction) const {
        JS_MarkValue(rt, _value, markFunction);
    }

    /**
     * Next Value member of the same owning object (walked by the GC mark
     * function).
     */
    Value                  *nextMember() const {
        return _nextMember;
    }

private:
    /**
     * Frees the reference; the value becomes undefined.
     */
    void release() {
        // through the runtime: works even when the context is gone
        if (_rt != nullptr) {
            JS_FreeValueRT(_rt, _value);
        }

        _value = JS_UNDEFINED;
    }

    /**
     * Registers this value in the member list of the object being constructed,
     * if it is one of its members.
     */
    void attach() {
        // not a member until proven otherwise
        _owner = nullptr;
        _nextMember = nullptr;
        _previousMember = nullptr;

        // a member lies inside the memory of the object under construction
        detail::Constructing *current = detail::constructing;
        const char *address = (const char *)this;
        bool isMember = current != nullptr && address >= current->begin && address < current->end;
        if (!isMember) {
            return;
        }

        // push it at the front of the object's member list
        _owner = current->header;
        _nextMember = _owner->firstMember;
        if (_nextMember != nullptr) {
            _nextMember->_previousMember = this;
        }

        _owner->firstMember = this;
    }

    /**
     * Unlinks this value from the member list of its owning object, if any.
     */
    void detach() {
        // not a member: nothing to unlink
        if (_owner == nullptr) {
            return;
        }

        // unlink from the previous member, or from the head of the list
        if (_previousMember != nullptr) {
            _previousMember->_nextMember = _nextMember;
        } else {
            _owner->firstMember = _nextMember;
        }

        // and from the next member
        if (_nextMember != nullptr) {
            _nextMember->_previousMember = _previousMember;
        }

        _owner = nullptr;
    }

    JSContext              *_ctx;             // context of the value; nullptr: none
    JSRuntime              *_rt;              // runtime of _ctx, frees the value; nullptr: none
    JSValue                 _value;           // owned
    detail::InstanceHeader *_owner;           // bound object this is a member of; nullptr: none
    Value                  *_nextMember;      // next in the owner's member list
    Value                  *_previousMember;  // previous in that list; nullptr: first
};

// A JS function. Calling it returns the result (undefined if the call
// failed: the JS exception is then recorded in the current call, see
// jsbind_error.h, or reported as uncaught if there is no current call).
class Function : public Value {
public:
    Function() = default;

    /**
     * Takes ownership of 'owned', which must be a function.
     */
    Function(JSContext *ctx, JSValue owned) : Value(ctx, owned) {}

    /**
     * Calls the function with 'this' undefined (defined in jsbind_convert.h).
     */
    template<class... A> Value operator()(const A &...arguments) const;

    /**
     * Calls the function with the given 'this' (defined in jsbind_convert.h).
     */
    template<class... A> Value callWith(JSValueConst thisValue, const A &...arguments) const;
};

/**
 * Read-only UTF-8 string with a size: either a view on C++ text (not owned)
 * or a string received from JS (owned, freed with the Str). Move-only.
 */
class Str {
public:
    /**
     * Empty string.
     */
    Str() : _data(""), _size(0), _ctx(nullptr), _owned(nullptr) {}

    /**
     * View on a C++ null-terminated text (not copied, must outlive the Str).
     */
    Str(const char *text) : _data(text), _size(strlen(text)), _ctx(nullptr), _owned(nullptr) {}

    /**
     * View on 'size' bytes of C++ text (not copied, must outlive the Str).
     */
    Str(const char *text, size_t size) : _data(text), _size(size), _ctx(nullptr), _owned(nullptr) {}

    /**
     * String received from JS; freed with the Str.
     */
    static Str fromJS(JSContext *ctx, const char *text, size_t size) {
        Str result(text, size);
        result._ctx = ctx;
        result._owned = text;
        return result;
    }

    /**
     * Move: takes the ownership of the text of 'other'.
     */
    Str(Str &&other) : _data(other._data), _size(other._size), _ctx(other._ctx), _owned(other._owned) {
        other._owned = nullptr;
    }

    /**
     * Move assignment: frees the current text, takes the one of 'other'.
     */
    Str &operator=(Str &&other) {
        if (this != &other) {
            releaseText();
            _data = other._data;
            _size = other._size;
            _ctx = other._ctx;
            _owned = other._owned;
            other._owned = nullptr;
        }

        return *this;
    }

    Str(const Str &) = delete;
    Str &operator=(const Str &) = delete;

    /**
     * Frees the text if it came from JS.
     */
    ~Str() {
        releaseText();
    }

    /**
     * The bytes (null-terminated when they come from JS or from a C string).
     */
    const char *data() const {
        return _data;
    }

    /**
     * Number of bytes, without the terminating null.
     */
    size_t      size() const {
        return _size;
    }

    /**
     * True for the empty string.
     */
    bool        empty() const {
        return _size == 0;
    }

    /**
     * True if the string has exactly the bytes of 'text'.
     */
    bool        equals(const char *text) const {
        return strlen(text) == _size && memcmp(text, _data, _size) == 0;
    }

private:
    /**
     * Gives the text back to QuickJS if it came from JS.
     */
    void releaseText() {
        if (_owned != nullptr) {
            JS_FreeCString(_ctx, _owned);
            _owned = nullptr;
        }
    }

    const char *_data;   // the bytes (UTF-8), borrowed or == _owned
    size_t      _size;   // bytes, without the terminating 0
    JSContext  *_ctx;    // frees _owned; nullptr: text not from JS
    const char *_owned;  // C string from JS, freed with the Str; nullptr: none
};

/**
 * Bytes: received from JS, a view on the memory of an ArrayBuffer or typed
 * array (valid during the call; the buffer is held by '_keep'); given to JS,
 * copied into a new Uint8Array.
 */
class Bytes {
public:
    /**
     * No bytes.
     */
    Bytes() : _data(nullptr), _size(0) {}

    /**
     * View on C++ memory (not copied, must outlive the Bytes).
     */
    Bytes(const uint8_t *data, size_t size) : _data((uint8_t *)data), _size(size) {}

    /**
     * View on JS memory; 'keep' holds the buffer alive while the view exists.
     */
    Bytes(uint8_t *data, size_t size, Value keep) : _data(data), _size(size), _keep(meta::move(keep)) {}

    /**
     * First byte (nullptr when empty and default-constructed).
     */
    uint8_t *data() const {
        return _data;
    }

    /**
     * Number of bytes.
     */
    size_t   size() const {
        return _size;
    }

private:
    uint8_t *_data;  // first byte, not owned; nullptr: none
    size_t   _size;  // bytes
    Value    _keep;  // JS buffer holding the bytes; undefined for C++ memory
};

// A Uint8Array owned (shared) by C++.
class Buffer {
public:
    /**
     * No array.
     */
    Buffer() : _data(nullptr), _size(0) {}

    /**
     * Takes 'array' (a typed array or ArrayBuffer) whose bytes are
     * 'data'..'data + size'.
     */
    Buffer(Value array, uint8_t *data, size_t size) : _array(meta::move(array)), _data(data), _size(size) {}

    /**
     * New zero-filled Uint8Array of 'size' bytes (in the current call's
     * context, or in 'ctx').
     */
    static Buffer create(size_t size, JSContext *ctx = nullptr);

    /**
     * New Uint8Array holding a copy of 'size' bytes at 'data' (in the
     * current call's context, or in 'ctx').
     */
    static Buffer copy(const void *data, size_t size, JSContext *ctx = nullptr);

    /**
     * First byte of the array (writable: C++ and JS share the memory).
     */
    uint8_t      *data() const {
        return _data;
    }

    /**
     * Number of bytes.
     */
    size_t        size() const {
        return _size;
    }

    /**
     * The JS array itself.
     */
    const Value  &value() const {
        return _array;
    }

private:
    Value         _array;  // the typed array or ArrayBuffer; undefined: none
    uint8_t      *_data;   // its bytes (shared with JS); nullptr: none
    size_t        _size;   // bytes
};

/**
 * A value that may be absent: an argument or field missing or undefined in JS.
 */
template<class T>
class Optional {
public:
    /**
     * Absent value.
     */
    Optional() : _present(false), _value() {}

    /**
     * Present value.
     */
    Optional(T value) : _present(true), _value(meta::move(value)) {}

    /**
     * True if the value is present.
     */
    bool     has() const {
        return _present;
    }

    /**
     * The value (default-constructed when absent).
     */
    const T &value() const {
        return _value;
    }

    /**
     * The value (default-constructed when absent), modifiable.
     */
    T       &value() {
        return _value;
    }

    /**
     * The value if present, else 'fallback'.
     */
    T        valueOr(T fallback) const {
        return _present ? _value : fallback;
    }

    /**
     * Stores a value; it becomes present.
     */
    void set(T value) {
        _value = meta::move(value);
        _present = true;
    }

private:
    bool _present;  // false: absent
    T    _value;    // default-constructed when absent
};

/**
 * A number constrained to [Lo, Hi]: the conversion from JS rejects the
 * values outside with a RangeError. Used like a T.
 */
template<class T, long long Lo, long long Hi>
struct Range {
    T value;  // the number

    /**
     * Zero (not checked against the bounds).
     */
    Range() : value() {}

    /**
     * Given value (not checked against the bounds).
     */
    Range(T initial) : value(initial) {}

    /**
     * Reads the number.
     */
    operator T() const {
        return value;
    }

    static constexpr long long lowest = Lo;   // smallest allowed value
    static constexpr long long highest = Hi;  // largest allowed value
};

// All the remaining arguments (must be the last parameter).
struct Args {
    JSContext    *ctx;     // context of the call
    int           count;   // number of remaining arguments
    JSValueConst *values;  // the arguments; borrowed

    /**
     * Borrowed argument 'index' (0 = first remaining one), undefined past
     * the end.
     */
    JSValueConst operator[](int index) const {
        return index < count ? values[index] : JS_UNDEFINED;
    }
};

} // namespace js
