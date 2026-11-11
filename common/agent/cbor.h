// CBOR subset of the agent protocol (same as host/js/cbor.js):
// unsigned/negative integers, byte and text strings, arrays, maps, false,
// true, null, undefined, floats (written on 64 bits; read on 16, 32 or 64).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cbor {

// Growing output buffer. Containers are written as a head (array/map with
// its item count) followed by the items, written by the caller.
class Writer {
public:
    /**
     * Empty writer; the buffer is allocated on the first write.
     */
    Writer();

    /**
     * Frees the buffer.
     */
    ~Writer();

    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;

    /**
     * Head of an array of 'count' items (the items follow).
     */
    void     array(uint64_t count);

    /**
     * Head of a map of 'count' key/value pairs (the pairs follow).
     */
    void     map(uint64_t count);

    /**
     * Integer, unsigned or negative major type depending on its sign.
     */
    void     integer(int64_t value);

    /**
     * Null-terminated UTF-8 text string.
     */
    void     text(const char *text);

    /**
     * UTF-8 text string of 'length' bytes.
     */
    void     text(const char *text, size_t length);

    /**
     * Byte string.
     */
    void     bytes(const uint8_t *data, size_t size);

    /**
     * true or false.
     */
    void     boolean(bool value);

    /**
     * null.
     */
    void     null();

    /**
     * undefined.
     */
    void     undefined();

    /**
     * Float, always on 64 bits.
     */
    void     floating(double value);

    /**
     * Appends values already encoded by another writer.
     */
    void           encoded(const uint8_t *data, size_t size) {
        append(data, size);
    }

    /**
     * The encoded bytes (nullptr while empty), owned by the writer.
     */
    const uint8_t *data() const {
        return _data;
    }

    /**
     * Number of encoded bytes.
     */
    size_t         size() const {
        return _size;
    }

private:
    /**
     * Writes the initial byte and argument of an item.
     */
    void     head(int major, uint64_t value);

    /**
     * Appends raw bytes, growing the buffer.
     */
    void     append(const uint8_t *data, size_t size);

    uint8_t *_data;       // malloc'd buffer; nullptr until the first write
    size_t   _size;       // bytes written
    size_t   _capacity;   // bytes allocated
};

enum class Type { Unsigned, Negative, Bytes, Text, Array, Map, False, True, Null, Undefined, Float, Invalid };

// Reads a buffer item by item. Every read returns false on a type mismatch
// or truncated data. The buffer is borrowed: it must outlive the reader and
// the strings it returns.
class Reader {
public:
    /**
     * Reader at the start of 'data' (borrowed).
     */
    Reader(const uint8_t *data, size_t size);

    /**
     * Type of the next item, without reading it (Invalid at the end or for
     * an unsupported item).
     */
    Type           peek() const;

    /**
     * Unsigned or negative integer (above INT64_MAX: wrong value).
     */
    bool           integer(int64_t *value);

    /**
     * Text string; points into the buffer (not null terminated).
     */
    bool           text(const char **text, size_t *length);

    /**
     * Byte string; points into the buffer.
     */
    bool           bytes(const uint8_t **data, size_t *size);

    /**
     * Head of an array: its item count (the items follow).
     */
    bool           array(uint64_t *count);

    /**
     * Head of a map: its pair count (the pairs follow).
     */
    bool           map(uint64_t *count);

    /**
     * null.
     */
    bool           null();

    /**
     * undefined.
     */
    bool           undefined();

    /**
     * true or false.
     */
    bool           boolean(bool *value);

    /**
     * Float on 16, 32 or 64 bits.
     */
    bool           floating(double *value);

    /**
     * Skips any value, nested ones included (at most MAX_DEPTH levels: deeper data is
     * refused, it would exhaust the stack of a small RTOS task).
     */
    bool           skip();

    static const int MAX_DEPTH = 16;   // deepest nesting skip() accepts

    /**
     * True when the whole buffer has been read.
     */
    bool           atEnd() const {
        return _position == _size;
    }

    /**
     * Current position: the next value starts at data() + position().
     */
    size_t         position() const {
        return _position;
    }

    /**
     * The buffer being read.
     */
    const uint8_t *data() const {
        return _data;
    }

private:
    /**
     * Reads the initial byte and argument (count, length or value) of an item. False at the
     * end of data, on a truncated argument or an indefinite length; the position moves anyway.
     */
    bool           head(int *major, uint64_t *argument);

    /**
     * True if 'size' bytes remain after the position. Never overflows, whatever 'size'
     * (a length read from the wire can be anything).
     */
    bool           fits(uint64_t size) const {
        return size <= _size - _position;
    }

    /**
     * skip() at a nesting depth.
     */
    bool           skipAt(int depth);

    const uint8_t *_data;       // borrowed
    size_t         _size;       // bytes of _data
    size_t         _position;   // offset of the next item
};

} // namespace cbor
