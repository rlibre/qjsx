// SHA-256 (FIPS 180-4), shared by the crypto module and the device agent.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace crypto {

// Use: update() any number of times, then finish() once (the object is not
// reset; make a new one for the next hash).
struct Sha256 {
    uint32_t state[8];   // current hash value (H0..H7)
    uint64_t length;     // bytes processed
    uint8_t  block[64];  // bytes of the block being filled
    size_t   blockSize;  // bytes used in block (< 64)

    /**
     * Initial hash value of FIPS 180-4, nothing processed.
     */
    Sha256() {
        static const uint32_t initial[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
        };
        memcpy(state, initial, sizeof(state));
        length = 0;
        blockSize = 0;
    }

    /**
     * 32-bit right rotation (0 < count < 32).
     */
    static uint32_t rotateRight(uint32_t value, int count) {
        return (value >> count) | (value << (32 - count));
    }

    /**
     * Processes one 64-byte block into 'state' (FIPS 180-4, 6.2.2).
     */
    void compress(const uint8_t *data) {
        // round constants (FIPS 180-4, 4.2.2)
        static const uint32_t constants[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };
        uint32_t schedule[64];

        // message schedule: the 16 big-endian words of the block...
        for (int i = 0; i < 16; i++) {
            schedule[i] = (uint32_t)data[i * 4] << 24 | (uint32_t)data[i * 4 + 1] << 16
                        | (uint32_t)data[i * 4 + 2] << 8 | (uint32_t)data[i * 4 + 3];
        }

        // ...extended to 64 words
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotateRight(schedule[i - 15], 7) ^ rotateRight(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
            uint32_t s1 = rotateRight(schedule[i - 2], 17) ^ rotateRight(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
            schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
        }

        // 64 rounds on the working variables a..h, starting from the state
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

        for (int i = 0; i < 64; i++) {
            uint32_t sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            uint32_t choose = (e & f) ^ (~e & g);
            uint32_t temp1 = h + sum1 + choose + constants[i] + schedule[i];
            uint32_t sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        // add the working variables to the state
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
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
     * Writes the 32-byte digest. Once only: the object is not reset.
     */
    void finish(uint8_t digest[32]) {
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
        for (int i = 0; i < 8; i++) {
            digest[i * 4] = (uint8_t)(state[i] >> 24);
            digest[i * 4 + 1] = (uint8_t)(state[i] >> 16);
            digest[i * 4 + 2] = (uint8_t)(state[i] >> 8);
            digest[i * 4 + 3] = (uint8_t)(state[i]);
        }
    }
};

} // namespace crypto
