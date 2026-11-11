// Framing of the agent protocol (same as host/js/frames.js):
//   0x00 | COBS( 0xE5 | version 1 | CRC-16/CCITT-FALSE (big endian) | message ) | 0x00
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace frames {

const uint8_t MAGIC = 0xe5;
const uint8_t VERSION = 1;

/**
 * CRC-16/CCITT-FALSE of 'data' (polynomial 0x1021, initial value 0xffff).
 */
uint16_t crc16(const uint8_t *data, size_t size);

/**
 * Frame of 'message', delimiters included; *frameSize receives its size.
 * Malloc'd: the caller frees it.
 */
uint8_t *encode(const uint8_t *message, size_t size, size_t *frameSize);

// Splits a byte stream into messages and text (bytes that are not frames,
// e.g. console output on a serial link). Not thread safe: one thread feeds it.
class Parser {
public:
    typedef void MessageCallback(void *opaque, const uint8_t *message, size_t size);
    typedef void TextCallback(void *opaque, const uint8_t *text, size_t size);

    /**
     * Keeps up to 'maxFrame' bytes between two 0x00 (longer segments are dropped); 'onText' may
     * be null (text dropped). Callbacks run inside feed(); their data is valid during the call only.
     */
    Parser(size_t maxFrame, MessageCallback *onMessage, TextCallback *onText, void *opaque);

    /**
     * Frees the segment buffer.
     */
    ~Parser();

    Parser(const Parser &) = delete;
    Parser &operator=(const Parser &) = delete;

    /**
     * Bytes received, any cut: each 0x00 ends a segment, given to onMessage (valid frame) or
     * onText. Bytes after the last 0x00 wait for the next call.
     */
    void             feed(const uint8_t *data, size_t size);

private:
    /**
     * Handles the segment collected since the last 0x00, then empties it.
     */
    void             flushSegment();

    uint8_t         *_segment;     // malloc'd; bytes since the last 0x00
    size_t           _length;      // bytes in _segment
    size_t           _capacity;    // size of _segment (maxFrame)
    bool             _overflow;    // segment too long: dropped at the next 0x00
    MessageCallback *_onMessage;   // called for each valid frame
    TextCallback    *_onText;      // nullptr: text dropped
    void            *_opaque;      // given to the callbacks
};

} // namespace frames
