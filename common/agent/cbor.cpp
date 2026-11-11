/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

#include "cbor.h"
#include "memory.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace cbor {

/**
 * Empty writer: no buffer until the first write.
 */
Writer::Writer() : _data(nullptr), _size(0), _capacity(0) {}

/**
 * Frees the buffer.
 */
Writer::~Writer() {
    memory::release(_data);
}

/**
 * Appends raw bytes at the end of the buffer, growing it if needed.
 */
void Writer::append(const uint8_t *data, size_t size) {
    // no room left: grow the buffer, 64 bytes first, then doubled until the
    // new bytes fit
    if (_size + size > _capacity) {
        size_t capacity = _capacity == 0 ? 64 : _capacity;

        while (capacity < _size + size) {
            capacity *= 2;
        }

        _data = (uint8_t *)memory::reallocate(_data, capacity);
        _capacity = capacity;
    }

    // copy the bytes after the ones already written
    memcpy(_data + _size, data, size);
    _size += size;
}

/**
 * Writes the head of an item: major type and argument, in the shortest form.
 */
void Writer::head(int major, uint64_t value) {
    // the major type goes in the 3 high bits of the initial byte
    uint8_t buffer[9];
    uint8_t type = (uint8_t)(major << 5);
    size_t length = 0;

    if (value < 24) {
        // a small argument fits in the 5 low bits
        buffer[length++] = (uint8_t)(type | value);
    } else {
        // otherwise info 24..27 announces 1, 2, 4 or 8 bytes: the fewest
        // that hold the value
        int byteCount = value < 0x100 ? 1 : value < 0x10000 ? 2 : value < 0x100000000ull ? 4 : 8;
        int info = byteCount == 1 ? 24 : byteCount == 2 ? 25 : byteCount == 4 ? 26 : 27;
        buffer[length++] = (uint8_t)(type | info);

        // then those bytes, big endian
        for (int i = byteCount - 1; i >= 0; i--) {
            buffer[length++] = (uint8_t)(value >> (i * 8));
        }
    }

    // append the head in one go
    append(buffer, length);
}

/**
 * Writes an array head: major type 4 with the item count.
 */
void Writer::array(uint64_t count) {
    head(4, count);
}

/**
 * Writes a map head: major type 5 with the pair count.
 */
void Writer::map(uint64_t count) {
    head(5, count);
}

/**
 * Writes an integer: major type 0 if positive, 1 if negative.
 */
void Writer::integer(int64_t value) {
    // CBOR stores a negative value n as -1 - n (so -1 is 0): the
    // subtraction never overflows, even for INT64_MIN
    if (value >= 0) {
        head(0, (uint64_t)value);
    } else {
        head(1, (uint64_t)(-1 - value));
    }
}

/**
 * Writes a null-terminated text string (without its terminator).
 */
void Writer::text(const char *text) {
    this->text(text, strlen(text));
}

/**
 * Writes a text string: major type 3 with the length, then the bytes.
 */
void Writer::text(const char *text, size_t length) {
    head(3, length);
    append((const uint8_t *)text, length);
}

/**
 * Writes a byte string: major type 2 with the size, then the bytes.
 */
void Writer::bytes(const uint8_t *data, size_t size) {
    head(2, size);
    append(data, size);
}

/**
 * Writes true or false: simple values 0xf5 / 0xf4.
 */
void Writer::boolean(bool value) {
    uint8_t byte = value ? 0xf5 : 0xf4;
    append(&byte, 1);
}

/**
 * Writes null: simple value 0xf6.
 */
void Writer::null() {
    uint8_t byte = 0xf6;
    append(&byte, 1);
}

/**
 * Writes undefined: simple value 0xf7.
 */
void Writer::undefined() {
    uint8_t byte = 0xf7;
    append(&byte, 1);
}

/**
 * Writes a float, always as a 64-bit double.
 */
void Writer::floating(double value) {
    // the IEEE 754 bits of the double (memcpy: no aliasing issue)
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));

    // initial byte, then the 8 bytes big endian
    uint8_t buffer[9];
    buffer[0] = 0xfb;   // major type 7, 64-bit float

    for (int i = 0; i < 8; i++) {
        buffer[1 + i] = (uint8_t)(bits >> ((7 - i) * 8));
    }

    append(buffer, sizeof(buffer));
}

/**
 * Starts at the beginning of the buffer.
 */
Reader::Reader(const uint8_t *data, size_t size) : _data(data), _size(size), _position(0) {}

/**
 * Type of the next item, read from its initial byte without moving.
 */
Type Reader::peek() const {
    // nothing left: no item
    if (_position >= _size) {
        return Type::Invalid;
    }

    // the initial byte: major type in the 3 high bits, additional info in the 5 low bits
    uint8_t initial = _data[_position];

    // classify by major type; indefinite lengths (info 31) are not checked
    // here, head() rejects them; tags (major type 6) are not supported
    switch (initial >> 5) {
        case 0: {
            return Type::Unsigned;
        }
        case 1: {
            return Type::Negative;
        }
        case 2: {
            return Type::Bytes;
        }
        case 3: {
            return Type::Text;
        }
        case 4: {
            return Type::Array;
        }
        case 5: {
            return Type::Map;
        }
        case 7: {
            // major type 7: the additional info is the simple value, or the
            // size of a float (25, 26, 27: 16, 32, 64 bits)
            switch (initial & 0x1f) {
                case 20: {
                    return Type::False;
                }
                case 21: {
                    return Type::True;
                }
                case 22: {
                    return Type::Null;
                }
                case 23: {
                    return Type::Undefined;
                }
                case 25:
                case 26:
                case 27: {
                    return Type::Float;
                }
                default: {
                    return Type::Invalid;
                }
            }
        }
        default: {
            return Type::Invalid;
        }
    }
}

/**
 * Reads the head of an item: major type and argument. The position moves past the head,
 * even when it fails.
 */
bool Reader::head(int *major, uint64_t *argument) {
    // nothing left: no head
    if (_position >= _size) {
        return false;
    }

    // the initial byte: major type (3 high bits) and additional info (5 low
    // bits); an info below 24 is the argument itself
    uint8_t initial = _data[_position++];
    *major = initial >> 5;
    int info = initial & 0x1f;
    if (info < 24) {
        *argument = info;
        return true;
    }

    // 24..27 announce 1, 2, 4 or 8 more bytes, which must be there;
    // 28..31 (reserved, indefinite length) are rejected
    int byteCount = info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : 0;
    if (byteCount == 0 || _position + byteCount > _size) {
        return false;
    }

    // read those bytes, big endian
    uint64_t value = 0;

    for (int i = 0; i < byteCount; i++) {
        value = value << 8 | _data[_position++];
    }

    *argument = value;
    return true;
}

/**
 * Reads an integer: major type 0 (value) or 1 (-1 - value).
 */
bool Reader::integer(int64_t *value) {
    // the head must be an integer one
    int major = 0;
    uint64_t argument = 0;
    if (!head(&major, &argument) || (major != 0 && major != 1)) {
        return false;
    }

    // the argument is the value, or -1 - the value for a negative one
    *value = major == 0 ? (int64_t)argument : -1 - (int64_t)argument;
    return true;
}

/**
 * Reads a text string: points into the buffer.
 */
bool Reader::text(const char **text, size_t *length) {
    // a head of major type 3, whose length must fit in the rest of the buffer
    int major = 0;
    uint64_t argument = 0;
    if (!head(&major, &argument) || major != 3 || !fits(argument)) {
        return false;
    }

    // give the bytes where they are and move past them
    *text = (const char *)(_data + _position);
    *length = (size_t)argument;
    _position += (size_t)argument;
    return true;
}

/**
 * Reads a byte string: points into the buffer.
 */
bool Reader::bytes(const uint8_t **data, size_t *size) {
    // a head of major type 2, whose size must fit in the rest of the buffer
    int major = 0;
    uint64_t argument = 0;
    if (!head(&major, &argument) || major != 2 || !fits(argument)) {
        return false;
    }

    // give the bytes where they are and move past them
    *data = _data + _position;
    *size = (size_t)argument;
    _position += (size_t)argument;
    return true;
}

/**
 * Reads an array head (major type 4): its item count.
 */
bool Reader::array(uint64_t *count) {
    int major = 0;
    return head(&major, count) && major == 4;
}

/**
 * Reads a map head (major type 5): its pair count.
 */
bool Reader::map(uint64_t *count) {
    int major = 0;
    return head(&major, count) && major == 5;
}

/**
 * Reads null: one byte, if it is null.
 */
bool Reader::null() {
    if (peek() != Type::Null) {
        return false;
    }

    _position++;
    return true;
}

/**
 * Reads undefined: one byte, if it is undefined.
 */
bool Reader::undefined() {
    if (peek() != Type::Undefined) {
        return false;
    }

    _position++;
    return true;
}

/**
 * Reads true or false: one byte, if it is one of them.
 */
bool Reader::boolean(bool *value) {
    Type type = peek();
    if (type != Type::True && type != Type::False) {
        return false;
    }

    *value = type == Type::True;
    _position++;
    return true;
}

namespace {

/**
 * IEEE 754 half precision (CBOR major type 7, info 25) to double.
 */
double halfToDouble(uint16_t half) {
    // split the 16 bits: sign (1), exponent (5), mantissa (10)
    int sign = half >> 15;
    int exponent = (half >> 10) & 0x1f;
    int mantissa = half & 0x3ff;

    // exponent 0: subnormal; 31: infinity or NaN; otherwise normal, with
    // the implicit leading 1 (1024) and the bias (15, plus 10 mantissa bits)
    double magnitude;
    if (exponent == 0) {
        magnitude = ldexp(mantissa, -24);                     // subnormal
    } else if (exponent == 31) {
        magnitude = mantissa == 0 ? INFINITY : NAN;
    } else {
        magnitude = ldexp(mantissa + 1024, exponent - 25);
    }

    return sign ? -magnitude : magnitude;
}

} // namespace

/**
 * Reads a float of 16, 32 or 64 bits, as a double.
 */
bool Reader::floating(double *value) {
    // the next item must be a float
    if (peek() != Type::Float) {
        return false;
    }

    // its size comes from the additional info (25, 26 or 27: 2, 4 or 8
    // bytes), checked against the buffer end
    int info = _data[_position] & 0x1f;
    size_t byteCount = info == 25 ? 2 : info == 26 ? 4 : 8;
    if (_position + 1 + byteCount > _size) {
        return false;
    }

    // read the bits after the initial byte, big endian
    uint64_t bits = 0;

    for (size_t i = 0; i < byteCount; i++) {
        bits = bits << 8 | _data[_position + 1 + i];
    }

    // move past the item
    _position += 1 + byteCount;

    // convert: half precision by hand, single and double by copying the
    // bits into a float / double
    if (byteCount == 2) {
        *value = halfToDouble((uint16_t)bits);
    } else if (byteCount == 4) {
        uint32_t bits32 = (uint32_t)bits;
        float single = 0;
        memcpy(&single, &bits32, sizeof(single));
        *value = single;
    } else {
        memcpy(value, &bits, sizeof(*value));
    }

    return true;
}

/**
 * Moves past the next value, nested ones included. False on truncated data or an
 * unsupported item.
 */
bool Reader::skip() {
    return skipAt(0);
}

/**
 * skip() at a nesting depth: arrays and maps go one level deeper.
 */
bool Reader::skipAt(int depth) {
    // too deeply nested: refused, the recursion would exhaust a small stack
    if (depth > MAX_DEPTH) {
        return false;
    }

    // simple values: a single byte
    Type type = peek();
    if (type == Type::False || type == Type::True || type == Type::Null || type == Type::Undefined) {
        _position++;
        return true;
    }

    // floats: the initial byte, then 2, 4 or 8 bytes
    if (type == Type::Float) {
        int info = _data[_position] & 0x1f;
        size_t size = info == 25 ? 2 : info == 26 ? 4 : 8;
        if (!fits(1 + size)) {
            return false;
        }

        _position += 1 + size;
        return true;
    }

    // anything else starts with a head
    int major = 0;
    uint64_t argument = 0;
    if (!head(&major, &argument)) {
        return false;
    }

    // what follows the head depends on the major type
    switch (major) {
        case 0:
        case 1: {
            // integers: the head is the whole item
            return true;
        }
        case 2:
        case 3: {
            // strings: skip their bytes, which must be in the buffer
            if (!fits(argument)) {
                return false;
            }

            _position += (size_t)argument;
            return true;
        }
        case 4: {
            // arrays: skip each item, one level deeper (a huge count stops at the
            // end of the buffer)
            for (uint64_t i = 0; i < argument; i++) {
                if (!skipAt(depth + 1)) {
                    return false;
                }
            }

            return true;
        }
        case 5: {
            // maps: a key and a value per pair, one level deeper (counted by
            // pairs: 'argument * 2' could overflow)
            for (uint64_t i = 0; i < argument; i++) {
                bool pair = skipAt(depth + 1) && skipAt(depth + 1);
                if (!pair) {
                    return false;
                }
            }

            return true;
        }
        default: {
            // tags (major type 6) and unsupported simple values (major type 7)
            return false;
        }
    }
}

} // namespace cbor
