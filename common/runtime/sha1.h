/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// SHA-1 (FIPS 180-4), for the crypto module. Broken for signatures: only for
// what a protocol imposes (the WebSocket handshake, Sec-WebSocket-Accept).
// Same use as Sha256 (sha256.h): update() any number of times, then finish()
// once.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace crypto {

struct Sha1 {
    uint32_t state[5];   // current hash value (H0..H4)
    uint64_t length;     // bytes processed
    uint8_t  block[64];  // bytes of the block being filled
    size_t   blockSize;  // bytes used in block (< 64)

    /**
     * Initial hash value of FIPS 180-4, nothing processed.
     */
    Sha1() {
        static const uint32_t initial[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
        memcpy(state, initial, sizeof(state));
        length = 0;
        blockSize = 0;
    }

    /**
     * 32-bit left rotation (0 < count < 32).
     */
    static uint32_t rotateLeft(uint32_t value, int count) {
        return (value << count) | (value >> (32 - count));
    }

    /**
     * Processes one 64-byte block into 'state' (FIPS 180-4, 6.1.2).
     */
    void compress(const uint8_t *data) {
        uint32_t schedule[80];

        // message schedule: the 16 big-endian words of the block...
        for (int i = 0; i < 16; i++) {
            schedule[i] = (uint32_t)data[i * 4] << 24 | (uint32_t)data[i * 4 + 1] << 16
                        | (uint32_t)data[i * 4 + 2] << 8 | (uint32_t)data[i * 4 + 3];
        }

        // ...extended to 80 words
        for (int i = 16; i < 80; i++) {
            schedule[i] = rotateLeft(schedule[i - 3] ^ schedule[i - 8] ^ schedule[i - 14] ^ schedule[i - 16], 1);
        }

        // 80 rounds on the working variables a..e, starting from the state;
        // the function and the constant change every 20 rounds
        uint32_t a = state[0];
        uint32_t b = state[1];
        uint32_t c = state[2];
        uint32_t d = state[3];
        uint32_t e = state[4];

        for (int i = 0; i < 80; i++) {
            uint32_t mixed = 0;
            uint32_t constant = 0;
            if (i < 20) {
                mixed = (b & c) | (~b & d);
                constant = 0x5a827999;
            } else if (i < 40) {
                mixed = b ^ c ^ d;
                constant = 0x6ed9eba1;
            } else if (i < 60) {
                mixed = (b & c) | (b & d) | (c & d);
                constant = 0x8f1bbcdc;
            } else {
                mixed = b ^ c ^ d;
                constant = 0xca62c1d6;
            }

            uint32_t temp = rotateLeft(a, 5) + mixed + e + constant + schedule[i];
            e = d;
            d = c;
            c = rotateLeft(b, 30);
            b = a;
            a = temp;
        }

        // add the working variables to the state
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    }

    /**
     * Adds bytes to the message. A partial block waits for the next call.
     */
    void update(const uint8_t *data, size_t size) {
        // count them for the final length
        length += size;

        // fill 'block'; compress each full block at once
        while (size > 0) {
            size_t chunk = 64 - blockSize < size ? 64 - blockSize : size;
            memcpy(block + blockSize, data, chunk);
            blockSize += chunk;
            data += chunk;
            size -= chunk;
            if (blockSize == 64) {
                compress(block);
                blockSize = 0;
            }
        }
    }

    /**
     * Writes the 20-byte digest. Once only: the object is not reset.
     */
    void finish(uint8_t digest[20]) {
        // note the message length in bits now: update() below changes 'length'
        uint64_t bitLength = length * 8;

        // pad: one 0x80 byte, then zeros up to 56 bytes in the block
        uint8_t padding = 0x80;
        update(&padding, 1);
        uint8_t zero = 0;

        while (blockSize != 56) {
            update(&zero, 1);
        }

        // append the bit length (64 bits, big endian): the last block is
        // compressed
        uint8_t lengthBytes[8];

        for (int i = 0; i < 8; i++) {
            lengthBytes[i] = (uint8_t)(bitLength >> (56 - i * 8));
        }

        update(lengthBytes, 8);

        // write the state words big endian
        for (int i = 0; i < 5; i++) {
            digest[i * 4] = (uint8_t)(state[i] >> 24);
            digest[i * 4 + 1] = (uint8_t)(state[i] >> 16);
            digest[i * 4 + 2] = (uint8_t)(state[i] >> 8);
            digest[i * 4 + 3] = (uint8_t)(state[i]);
        }
    }
};

} // namespace crypto
