/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

#include "frames.h"
#include "memory.h"

#include <stdlib.h>
#include <string.h>

namespace frames {

/**
 * CRC-16/CCITT-FALSE of 'data' (polynomial 0x1021, initial value 0xffff).
 */
uint16_t crc16(const uint8_t *data, size_t size) {
    uint16_t crc = 0xffff;

    // bitwise, without table (small code on the ESP): for each byte
    for (size_t i = 0; i < size; i++) {
        // xor it into the high byte
        crc ^= (uint16_t)(data[i] << 8);

        // then 8 shifts, xoring the polynomial when the top bit falls out
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }

    return crc;
}

namespace {

/**
 * COBS encoding of 'data' into 'output' (size + size / 254 + 1 bytes at most), without
 * delimiters. Returns the encoded size.
 */
size_t cobsEncode(const uint8_t *data, size_t size, uint8_t *output) {
    // each block starts with a code byte = 1 + number of non-zero bytes that
    // follow; its slot is reserved now and filled when the block ends
    size_t codeIndex = 0;
    size_t write = 1;
    uint8_t code = 1;

    // for each byte of the data
    for (size_t i = 0; i < size; i++) {
        // a 0x00 ends the block: the code byte stands for it, a new block starts
        if (data[i] == 0) {
            output[codeIndex] = code;
            codeIndex = write++;
            code = 1;
            continue;
        }

        // a non-zero byte is copied; after 254 of them the block is full
        // (code 0xff, no 0x00 implied): close it and start a new one
        output[write++] = data[i];
        code++;
        if (code == 0xff) {
            output[codeIndex] = code;
            codeIndex = write++;
            code = 1;
        }
    }

    // close the last block
    output[codeIndex] = code;
    return write;
}

/**
 * In-place COBS decoding (the output is never longer than the input). False if the data is
 * not valid COBS (0x00 inside, block past the end).
 */
bool cobsDecode(uint8_t *data, size_t size, size_t *decodedSize) {
    // read and write in the same buffer: the write position never passes the read one
    size_t read = 0;
    size_t write = 0;

    // for each block
    while (read < size) {
        // its code byte: 1 + number of data bytes, all inside the buffer
        uint8_t code = data[read++];
        if (code == 0 || read + code - 1 > size) {
            return false;
        }

        // copy the code - 1 data bytes
        for (int i = 1; i < code; i++) {
            data[write++] = data[read++];
        }

        // then restore the 0x00 the code stands for, except after a full block
        // (0xff) and at the end
        if (code != 0xff && read < size) {
            data[write++] = 0;
        }
    }

    *decodedSize = write;
    return true;
}

} // namespace

/**
 * Frame of 'message', delimiters included; the caller frees it.
 */
uint8_t *encode(const uint8_t *message, size_t size, size_t *frameSize) {
    // build the body: magic, version, CRC of the message (big endian), the message
    size_t bodySize = size + 4;
    uint8_t *body = (uint8_t *)memory::allocate(bodySize);
    uint16_t crc = crc16(message, size);
    body[0] = MAGIC;
    body[1] = VERSION;
    body[2] = (uint8_t)(crc >> 8);
    body[3] = (uint8_t)crc;
    memcpy(body + 4, message, size);

    // COBS-encode it between two 0x00 delimiters, in a buffer sized for the worst
    // case (one code byte per 254 bytes plus one, and the 2 delimiters)
    uint8_t *frame = (uint8_t *)memory::allocate(bodySize + bodySize / 254 + 3);
    frame[0] = 0;
    size_t encoded = cobsEncode(body, bodySize, frame + 1);
    frame[encoded + 1] = 0;

    // the body is no longer needed; the frame goes to the caller
    memory::release(body);
    *frameSize = encoded + 2;
    return frame;
}

/**
 * Allocates the segment buffer of 'maxFrame' bytes.
 */
Parser::Parser(size_t maxFrame, MessageCallback *onMessage, TextCallback *onText, void *opaque)
    : _segment((uint8_t *)memory::allocate(maxFrame)), _length(0), _capacity(maxFrame), _overflow(false),
      _onMessage(onMessage), _onText(onText), _opaque(opaque) {
}

/**
 * Frees the segment buffer.
 */
Parser::~Parser() {
    memory::release(_segment);
}

/**
 * Splits the received bytes into segments ended by 0x00.
 */
void Parser::feed(const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        // a 0x00 ends the segment: handle it
        if (data[i] == 0) {
            flushSegment();
            continue;
        }

        // another byte is appended; when the buffer is full the segment only
        // gets marked as overflowing
        if (_length < _capacity) {
            _segment[_length++] = data[i];
        } else {
            _overflow = true;   // too long: dropped at the next 0x00
        }
    }
}

/**
 * Handles the segment between two 0x00: a message if it is a valid frame, text otherwise.
 */
void Parser::flushSegment() {
    // an overflowing segment is dropped
    if (_overflow) {
        _overflow = false;
        _length = 0;
        return;
    }

    // an empty one (two 0x00 in a row) is ignored
    if (_length == 0) {
        return;
    }

    // decoding works in place: keep a copy for the text case, only if the
    // text has a taker (no copy per frame otherwise)
    uint8_t *copy = nullptr;
    if (_onText != nullptr) {
        copy = (uint8_t *)memory::allocate(_length);
        memcpy(copy, _segment, _length);
    }

    // a frame is valid COBS with magic, version and a matching CRC: onMessage
    // gets the message after the 4-byte header; anything else goes to onText
    size_t decoded = 0;
    bool isFrame = cobsDecode(_segment, _length, &decoded) && decoded >= 4
        && _segment[0] == MAGIC && _segment[1] == VERSION
        && crc16(_segment + 4, decoded - 4) == ((_segment[2] << 8) | _segment[3]);
    if (isFrame) {
        _onMessage(_opaque, _segment + 4, decoded - 4);
    } else if (_onText != nullptr) {
        _onText(_opaque, copy, _length);
    }

    // free the copy (if any) and start a new segment
    memory::release(copy);
    _length = 0;
}

} // namespace frames
