/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Images of the displays (user, 2026-11-18): a PNG file of the project,
//
//   import logo from "./logo.png" with { type: "image" };
//
// is converted on the PC, by 'qjsx build' (qjsx:bytes) or by 'qjsx run
// script.js' (host/host.cpp), into the format the renderer of the displays
// reads in place (common/draw/image.h, which describes it): rows of runs,
// pixels of 16 bits (5 of red, 6 of green, 5 of blue, high byte first, as
// the display controllers take them), an alpha of 8 bits on the edges only.
//
// The PNG decoder is ours (no dependency, user): zlib / deflate (RFC 1950,
// 1951), the chunks and their CRC, the five row filters, every color type
// (grey, RGB, palette, grey + alpha, RGB + alpha) in every bit depth, the
// transparency of the 'tRNS' chunk. Interlaced images are refused. Ancillary
// chunks (gamma, color profile, text...) are ignored.
//
// The colors are dithered on their way to 16 bits (Floyd-Steinberg, user:
// always): a gradient shows no bands, at no cost on the device.
//
// Nothing here reads or writes a file: bytes in, bytes out.

// ### Inflate (RFC 1951) ######################################################

// The base length and the extra bits of the length symbols 257..285.
const LENGTH_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
const LENGTH_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];

// The base distance and the extra bits of the distance symbols 0..29.
const DISTANCE_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
    4097, 6145, 8193, 12289, 16385, 24577];
const DISTANCE_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];

// The order in which a dynamic block gives the lengths of the code lengths.
const CODE_LENGTH_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];

// The longest Huffman code, bits; the symbols that end a block and that
// start the lengths.
const MAX_CODE_BITS = 15;
const END_OF_BLOCK = 256;
const FIRST_LENGTH_SYMBOL = 257;

// Kinds of blocks (their 2-bit type).
const STORED_BLOCK = 0;
const FIXED_BLOCK = 1;
const DYNAMIC_BLOCK = 2;

// Reads a deflate stream bit by bit, least significant bit first.
class BitReader {
    #bytes;              // the stream
    #position;           // the next byte to take
    #buffer = 0;         // bits taken and not read yet, the next one lowest
    #count = 0;          // how many

    constructor(bytes, position) {
        this.#bytes = bytes;
        this.#position = position;
    }

    /**
     * The next 'count' bits (at most 16), the first one lowest.
     */
    bits(count) {
        // enough bits in the buffer, a byte at a time
        while (this.#count < count) {
            if (this.#position >= this.#bytes.length) {
                throw new Error("the compressed data ends too early");
            }
            this.#buffer |= this.#bytes[this.#position] << this.#count;
            this.#position++;
            this.#count += 8;
        }

        // the lowest 'count' of them
        const value = this.#buffer & ((1 << count) - 1);
        this.#buffer >>>= count;
        this.#count -= count;
        return value;
    }

    /**
     * Drops the bits left of the current byte: the next read starts on a
     * byte. Returns where that byte is.
     */
    alignToByte() {
        this.#buffer = 0;
        this.#count = 0;
        return this.#position;
    }

    /**
     * Moves past 'count' bytes read by the caller (after alignToByte()).
     */
    skipBytes(count) {
        this.#position += count;
    }
}

// A canonical Huffman code, from the code length of each symbol: how many
// codes each length has, and the symbols sorted by code.
class HuffmanCode {
    counts = new Uint16Array(MAX_CODE_BITS + 1);   // codes of each length (0 unused)
    symbols;                                       // the symbols, in the order of their codes

    constructor(lengths) {
        // how many codes of each length
        for (const length of lengths) {
            this.counts[length]++;
        }
        this.counts[0] = 0;

        // where the symbols of each length start, in code order
        const starts = new Uint16Array(MAX_CODE_BITS + 2);
        for (let length = 1; length <= MAX_CODE_BITS; length++) {
            starts[length + 1] = starts[length] + this.counts[length];
        }

        // the symbols, by length then by value (the canonical order)
        this.symbols = new Uint16Array(lengths.length);
        lengths.forEach((length, symbol) => {
            if (length !== 0) {
                this.symbols[starts[length]] = symbol;
                starts[length]++;
            }
        });
    }

    /**
     * Reads one symbol: the code is read a bit at a time, the first bit the
     * highest; the codes of one length are consecutive numbers, the first
     * one twice the number after the last code of the length before.
     */
    decode(reader) {
        let code = 0;          // the bits read, as a number
        let first = 0;         // the first code of the current length
        let index = 0;         // where the symbols of the current length start

        for (let length = 1; length <= MAX_CODE_BITS; length++) {
            code |= reader.bits(1);

            // a code of this length
            const count = this.counts[length];
            if (code - first < count) {
                return this.symbols[index + code - first];
            }

            // else one bit longer
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }

        throw new Error("invalid compressed data (no such code)");
    }
}

// The codes of the fixed blocks, made on first use.
let fixedCodes = null;

/**
 * The literal / length and distance codes of a fixed block (RFC 1951 3.2.6).
 */
function fixedHuffmanCodes() {
    if (fixedCodes === null) {
        const lengths = new Uint8Array(288);
        lengths.fill(8, 0, 144);
        lengths.fill(9, 144, 256);
        lengths.fill(7, 256, 280);
        lengths.fill(8, 280, 288);
        const distances = new Uint8Array(30).fill(5);
        fixedCodes = { literals: new HuffmanCode(lengths), distances: new HuffmanCode(distances) };
    }
    return fixedCodes;
}

/**
 * The codes a dynamic block gives in its header (RFC 1951 3.2.7).
 */
function dynamicHuffmanCodes(reader) {
    // how many of each kind
    const literalCount = reader.bits(5) + 257;
    const distanceCount = reader.bits(5) + 1;
    const codeLengthCount = reader.bits(4) + 4;

    // the code of the code lengths
    const codeLengthLengths = new Uint8Array(19);
    for (let index = 0; index < codeLengthCount; index++) {
        codeLengthLengths[CODE_LENGTH_ORDER[index]] = reader.bits(3);
    }
    const codeLengthCode = new HuffmanCode(codeLengthLengths);

    // the lengths of both codes, in one sequence: 0-15 a length, 16 the
    // previous one again 3-6 times, 17 and 18 zeros 3-10 and 11-138 times
    const lengths = new Uint8Array(literalCount + distanceCount);
    let index = 0;

    while (index < lengths.length) {
        const symbol = codeLengthCode.decode(reader);
        let value = 0;
        let repeat = 1;
        if (symbol < 16) {
            value = symbol;
        } else if (symbol === 16) {
            if (index === 0) {
                throw new Error("invalid compressed data (no length to repeat)");
            }
            value = lengths[index - 1];
            repeat = 3 + reader.bits(2);
        } else if (symbol === 17) {
            repeat = 3 + reader.bits(3);
        } else {
            repeat = 11 + reader.bits(7);
        }

        if (index + repeat > lengths.length) {
            throw new Error("invalid compressed data (too many code lengths)");
        }
        lengths.fill(value, index, index + repeat);
        index += repeat;
    }

    // a block must be able to end
    if (lengths[END_OF_BLOCK] === 0) {
        throw new Error("invalid compressed data (no end of block)");
    }

    return {
        literals: new HuffmanCode(lengths.subarray(0, literalCount)),
        distances: new HuffmanCode(lengths.subarray(literalCount)),
    };
}

// The output of inflate: exactly as many bytes as expected.
class Output {
    bytes;               // the data
    length = 0;          // bytes written

    constructor(size) {
        this.bytes = new Uint8Array(size);
    }

    /**
     * Fails when 'count' more bytes do not fit.
     */
    reserve(count) {
        if (this.length + count > this.bytes.length) {
            throw new Error("the image data is longer than its size says");
        }
    }
}

/**
 * Decodes the symbols of a compressed block until its end, with its codes.
 */
function inflateCodes(reader, codes, output) {
    const bytes = output.bytes;

    for (;;) {
        const symbol = codes.literals.decode(reader);

        // a literal byte
        if (symbol < END_OF_BLOCK) {
            output.reserve(1);
            bytes[output.length] = symbol;
            output.length++;
            continue;
        }

        // the end of the block
        if (symbol === END_OF_BLOCK) {
            return;
        }

        // a copy of earlier bytes: its length, then its distance
        const lengthIndex = symbol - FIRST_LENGTH_SYMBOL;
        if (lengthIndex >= LENGTH_BASE.length) {
            throw new Error("invalid compressed data (no such length)");
        }
        const length = LENGTH_BASE[lengthIndex] + reader.bits(LENGTH_EXTRA[lengthIndex]);

        const distanceIndex = codes.distances.decode(reader);
        if (distanceIndex >= DISTANCE_BASE.length) {
            throw new Error("invalid compressed data (no such distance)");
        }
        const distance = DISTANCE_BASE[distanceIndex] + reader.bits(DISTANCE_EXTRA[distanceIndex]);
        if (distance > output.length) {
            throw new Error("invalid compressed data (a copy from before the start)");
        }

        // byte by byte: the copy may overlap what it writes
        output.reserve(length);
        for (let index = 0; index < length; index++) {
            bytes[output.length] = bytes[output.length - distance];
            output.length++;
        }
    }
}

/**
 * Copies a stored block (no compression).
 */
function inflateStored(reader, data, output) {
    // its length and the check of it, on a byte
    const start = reader.alignToByte();
    if (start + 4 > data.length) {
        throw new Error("the compressed data ends too early");
    }
    const length = data[start] | (data[start + 1] << 8);
    const check = data[start + 2] | (data[start + 3] << 8);
    if ((length ^ 0xffff) !== check) {
        throw new Error("invalid compressed data (a stored block's length)");
    }

    // its bytes
    const first = start + 4;
    if (first + length > data.length) {
        throw new Error("the compressed data ends too early");
    }
    output.reserve(length);
    output.bytes.set(data.subarray(first, first + length), output.length);
    output.length += length;
    reader.skipBytes(4 + length);
}

/**
 * The Adler-32 checksum of 'bytes' (RFC 1950), as an unsigned number.
 */
function adler32(bytes) {
    const MODULUS = 65521;
    let low = 1;
    let high = 0;

    for (const byte of bytes) {
        low = (low + byte) % MODULUS;
        high = (high + low) % MODULUS;
    }

    return ((high << 16) | low) >>> 0;
}

/**
 * Decompresses zlib data (RFC 1950: a header, deflate blocks, an Adler-32
 * check) that must give exactly 'size' bytes.
 */
export function inflate(data, size) {
    // the header: deflate, no preset dictionary
    const isZlib = data.length >= 2 && (data[0] & 0x0f) === 8 && ((data[0] << 8) | data[1]) % 31 === 0;
    if (!isZlib) {
        throw new Error("the image data is not zlib data");
    }
    if ((data[1] & 0x20) !== 0) {
        throw new Error("the image data uses a preset dictionary");
    }

    // the blocks, until the last one
    const reader = new BitReader(data, 2);
    const output = new Output(size);
    let isLast = false;

    while (!isLast) {
        isLast = reader.bits(1) === 1;
        const type = reader.bits(2);
        if (type === STORED_BLOCK) {
            inflateStored(reader, data, output);
        } else if (type === FIXED_BLOCK) {
            inflateCodes(reader, fixedHuffmanCodes(), output);
        } else if (type === DYNAMIC_BLOCK) {
            inflateCodes(reader, dynamicHuffmanCodes(reader), output);
        } else {
            throw new Error("invalid compressed data (no such block type)");
        }
    }

    // all of it, and its checksum
    if (output.length !== size) {
        throw new Error("the image data is shorter than its size says");
    }
    const end = reader.alignToByte();
    if (end + 4 > data.length) {
        throw new Error("the compressed data ends too early");
    }
    const expected = ((data[end] << 24) | (data[end + 1] << 16) | (data[end + 2] << 8) | data[end + 3]) >>> 0;
    if (adler32(output.bytes) !== expected) {
        throw new Error("the image data is damaged (Adler-32)");
    }

    return output.bytes;
}

// ### PNG #####################################################################

const PNG_SIGNATURE = [137, 80, 78, 71, 13, 10, 26, 10];

// The largest image converted: wider than any display, small enough for the
// memory of the tool.
const MAX_SIDE = 4096;

// Color types of a PNG: their channels and allowed bit depths.
const GREY = 0;
const RGB = 2;
const PALETTE = 3;
const GREY_ALPHA = 4;
const RGB_ALPHA = 6;
const COLOR_TYPES = {
    [GREY]: { channels: 1, depths: [1, 2, 4, 8, 16] },
    [RGB]: { channels: 3, depths: [8, 16] },
    [PALETTE]: { channels: 1, depths: [1, 2, 4, 8] },
    [GREY_ALPHA]: { channels: 2, depths: [8, 16] },
    [RGB_ALPHA]: { channels: 4, depths: [8, 16] },
};

// The row filters (PNG 9.2).
const FILTER_NONE = 0;
const FILTER_SUB = 1;
const FILTER_UP = 2;
const FILTER_AVERAGE = 3;
const FILTER_PAETH = 4;

// The table of the CRC-32 of the chunks, made on first use.
let crcTable = null;

/**
 * The CRC-32 of 'bytes' (the one of PNG and zip), as an unsigned number.
 */
function crc32(bytes) {
    // the table: the CRC of each byte value
    if (crcTable === null) {
        crcTable = new Uint32Array(256);
        for (let value = 0; value < 256; value++) {
            let crc = value;
            for (let bit = 0; bit < 8; bit++) {
                crc = (crc & 1) !== 0 ? 0xedb88320 ^ (crc >>> 1) : crc >>> 1;
            }
            crcTable[value] = crc;
        }
    }

    // the bytes, through it
    let crc = 0xffffffff;
    for (const byte of bytes) {
        crc = crcTable[(crc ^ byte) & 0xff] ^ (crc >>> 8);
    }
    return (crc ^ 0xffffffff) >>> 0;
}

/**
 * The chunks of a PNG file, checked: [{ type, data }], from IHDR to IEND.
 */
function readChunks(bytes) {
    // the signature
    const hasSignature = bytes.length >= 8 && PNG_SIGNATURE.every((byte, index) => bytes[index] === byte);
    if (!hasSignature) {
        throw new Error("not a PNG file");
    }

    // each chunk: length, type, data, CRC of type and data
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const chunks = [];
    let position = 8;

    for (;;) {
        if (position + 12 > bytes.length) {
            throw new Error("the PNG file ends too early");
        }
        const length = view.getUint32(position);
        const end = position + 8 + length;
        if (end + 4 > bytes.length) {
            throw new Error("the PNG file ends too early");
        }

        const typed = bytes.subarray(position + 4, end);
        if (crc32(typed) !== view.getUint32(end)) {
            throw new Error("the PNG file is damaged (CRC of a chunk)");
        }

        const type = String.fromCharCode(...typed.subarray(0, 4));
        chunks.push({ type, data: bytes.subarray(position + 8, end) });
        position = end + 4;

        if (type === "IEND") {
            return chunks;
        }
    }
}

/**
 * The header of a PNG (IHDR), checked: { width, height, depth, colorType,
 * channels }.
 */
function readHeader(chunk) {
    if (chunk.type !== "IHDR" || chunk.data.length !== 13) {
        throw new Error("the PNG file does not start with its header");
    }

    const view = new DataView(chunk.data.buffer, chunk.data.byteOffset, 13);
    const header = {
        width: view.getUint32(0),
        height: view.getUint32(4),
        depth: chunk.data[8],
        colorType: chunk.data[9],
    };

    // a size the tool converts
    const isSized = header.width >= 1 && header.height >= 1 && header.width <= MAX_SIDE && header.height <= MAX_SIDE;
    if (!isSized) {
        throw new Error(`the image is ${header.width} x ${header.height} pixels: from 1 to ${MAX_SIDE} on each side`);
    }

    // a known color type, in one of its depths
    const colorType = COLOR_TYPES[header.colorType];
    if (colorType === undefined || !colorType.depths.includes(header.depth)) {
        throw new Error(`the PNG file has an unknown color type ${header.colorType} with ${header.depth} bits`);
    }
    header.channels = colorType.channels;

    // compression and filters of PNG 1.2; no interlacing
    if (chunk.data[10] !== 0 || chunk.data[11] !== 0) {
        throw new Error("the PNG file uses an unknown compression or filter method");
    }
    if (chunk.data[12] !== 0) {
        throw new Error("interlaced PNG files are not supported: save the image without interlacing");
    }

    return header;
}

/**
 * Undoes the filters of the rows, in place: 'data' is the inflated image,
 * each row its filter byte then 'rowBytes' bytes. Returns the rows without
 * their filter bytes.
 */
function unfilter(data, height, rowBytes, pixelBytes) {
    const rows = new Uint8Array(height * rowBytes);

    for (let row = 0; row < height; row++) {
        const filter = data[row * (rowBytes + 1)];
        const source = data.subarray(row * (rowBytes + 1) + 1, (row + 1) * (rowBytes + 1));
        const at = row * rowBytes;
        const previous = at - rowBytes;   // the row above, in 'rows' (none for the first)

        for (let index = 0; index < rowBytes; index++) {
            // the bytes left, above and above-left of this one (0 outside)
            const left = index >= pixelBytes ? rows[at + index - pixelBytes] : 0;
            const above = row > 0 ? rows[previous + index] : 0;
            const aboveLeft = row > 0 && index >= pixelBytes ? rows[previous + index - pixelBytes] : 0;

            let predicted = 0;
            if (filter === FILTER_NONE) {
                predicted = 0;
            } else if (filter === FILTER_SUB) {
                predicted = left;
            } else if (filter === FILTER_UP) {
                predicted = above;
            } else if (filter === FILTER_AVERAGE) {
                predicted = (left + above) >> 1;
            } else if (filter === FILTER_PAETH) {
                // the neighbour closest to left + above - aboveLeft
                const estimate = left + above - aboveLeft;
                const toLeft = Math.abs(estimate - left);
                const toAbove = Math.abs(estimate - above);
                const toAboveLeft = Math.abs(estimate - aboveLeft);
                predicted = toLeft <= toAbove && toLeft <= toAboveLeft ? left : toAbove <= toAboveLeft ? above : aboveLeft;
            } else {
                throw new Error(`the PNG file uses an unknown row filter ${filter}`);
            }

            rows[at + index] = (source[index] + predicted) & 0xff;
        }
    }

    return rows;
}

/**
 * Sample 'index' of a row of samples of 'depth' bits (1, 2, 4, 8 or 16).
 */
function sampleOf(row, index, depth) {
    if (depth === 16) {
        return (row[index * 2] << 8) | row[index * 2 + 1];
    }
    if (depth === 8) {
        return row[index];
    }

    // several samples in a byte, the first one highest
    const bit = index * depth;
    const shift = 8 - depth - (bit & 7);
    return (row[bit >> 3] >> shift) & ((1 << depth) - 1);
}

/**
 * A sample of 'depth' bits as 8 bits.
 */
function to8Bits(sample, depth) {
    if (depth === 16) {
        return sample >> 8;
    }
    return Math.round((sample * 255) / ((1 << depth) - 1));
}

/**
 * The palette (PLTE) with the alpha of its entries (tRNS), as RGBA
 * quadruples; null without a PLTE chunk.
 */
function readPalette(chunks) {
    const palette = chunks.find(chunk => chunk.type === "PLTE");
    if (palette === undefined) {
        return null;
    }

    // RGB of each entry, opaque unless tRNS says otherwise
    const count = Math.floor(palette.data.length / 3);
    const colors = new Uint8Array(count * 4);
    for (let entry = 0; entry < count; entry++) {
        colors.set(palette.data.subarray(entry * 3, entry * 3 + 3), entry * 4);
        colors[entry * 4 + 3] = 255;
    }

    const transparency = chunks.find(chunk => chunk.type === "tRNS");
    if (transparency !== undefined) {
        for (let entry = 0; entry < Math.min(count, transparency.data.length); entry++) {
            colors[entry * 4 + 3] = transparency.data[entry];
        }
    }

    return colors;
}

/**
 * The transparent sample values of a grey or RGB image (tRNS: one value of
 * 16 bits per channel), or null.
 */
function readTransparentColor(chunks, channels) {
    const transparency = chunks.find(chunk => chunk.type === "tRNS");
    if (transparency === undefined || transparency.data.length < channels * 2) {
        return null;
    }

    const values = [];
    for (let channel = 0; channel < channels; channel++) {
        values.push((transparency.data[channel * 2] << 8) | transparency.data[channel * 2 + 1]);
    }
    return values;
}

/**
 * Decodes a PNG file: { width, height, rgba }, 'rgba' 4 bytes per pixel,
 * row by row. Throws an Error that says why it cannot.
 */
export function decodePng(bytes) {
    // the chunks: the header first; an unknown critical chunk is refused
    const chunks = readChunks(bytes);
    const header = readHeader(chunks[0]);
    for (const chunk of chunks) {
        const isCritical = chunk.type.charCodeAt(0) < 0x61;
        const isKnown = ["IHDR", "PLTE", "IDAT", "IEND"].includes(chunk.type);
        if (isCritical && !isKnown) {
            throw new Error(`the PNG file has an unknown critical chunk '${chunk.type}'`);
        }
    }

    // the image data: every IDAT, one zlib stream
    const parts = chunks.filter(chunk => chunk.type === "IDAT").map(chunk => chunk.data);
    const compressed = new Uint8Array(parts.reduce((total, part) => total + part.length, 0));
    let offset = 0;
    for (const part of parts) {
        compressed.set(part, offset);
        offset += part.length;
    }

    // its rows, unfiltered
    const { width, height, depth, colorType, channels } = header;
    const rowBytes = Math.ceil((width * channels * depth) / 8);
    const pixelBytes = Math.max(1, (channels * depth) >> 3);
    const data = inflate(compressed, height * (rowBytes + 1));
    const rows = unfilter(data, height, rowBytes, pixelBytes);

    // what the color type needs besides the samples
    const palette = colorType === PALETTE ? readPalette(chunks) : null;
    if (colorType === PALETTE && palette === null) {
        throw new Error("the PNG file has a palette image without its palette");
    }
    const transparent = colorType === GREY || colorType === RGB ? readTransparentColor(chunks, channels) : null;

    // each pixel as RGBA, 8 bits a channel
    const rgba = new Uint8Array(width * height * 4);

    for (let y = 0; y < height; y++) {
        const row = rows.subarray(y * rowBytes, (y + 1) * rowBytes);

        for (let x = 0; x < width; x++) {
            const out = (y * width + x) * 4;
            const first = x * channels;

            if (colorType === PALETTE) {
                // its entry (one past the palette: black, transparent)
                const entry = sampleOf(row, x, depth);
                if (entry * 4 < palette.length) {
                    rgba.set(palette.subarray(entry * 4, entry * 4 + 4), out);
                }
                continue;
            }

            // its samples, then as 8 bits
            const samples = [];
            for (let channel = 0; channel < channels; channel++) {
                samples.push(sampleOf(row, first + channel, depth));
            }

            const isGrey = colorType === GREY || colorType === GREY_ALPHA;
            rgba[out] = to8Bits(samples[0], depth);
            rgba[out + 1] = to8Bits(isGrey ? samples[0] : samples[1], depth);
            rgba[out + 2] = to8Bits(isGrey ? samples[0] : samples[2], depth);

            // alpha: its channel, or the transparent color of tRNS
            if (colorType === GREY_ALPHA) {
                rgba[out + 3] = to8Bits(samples[1], depth);
            } else if (colorType === RGB_ALPHA) {
                rgba[out + 3] = to8Bits(samples[3], depth);
            } else {
                const isTransparent = transparent !== null && transparent.every((value, channel) => value === samples[channel]);
                rgba[out + 3] = isTransparent ? 0 : 255;
            }
        }
    }

    return { width, height, rgba };
}

// ### The format of the renderer ##############################################
// (common/draw/image.h describes it; both sides must agree)

const IMAGE_MAGIC = [0x51, 0x49, 0x4d, 0x47];   // "QIMG"
const IMAGE_VERSION = 1;
const IMAGE_HEADER_BYTES = 12;
const ROW_OFFSET_BYTES = 4;

// The kinds of runs (the 2 high bits of a run's first byte; the 6 low ones
// are its length less one).
const RUN_SKIP = 0;          // transparent pixels: nothing follows
const RUN_OPAQUE = 1;        // pixels that cover: 2 bytes each follow
const RUN_BLENDED = 2;       // pixels partly transparent: 2 bytes and an alpha each
const RUN_REPEAT = 3;        // one pixel that covers, repeated: its 2 bytes follow
const RUN_MAX = 64;          // the longest run

// The shortest repeat worth a run of its own (3 bytes instead of 2 a pixel).
const REPEAT_MIN = 3;

/**
 * Dithers the colors to 16 bits (5 of red, 6 of green, 5 of blue) with
 * Floyd-Steinberg: the error of each pixel is spread over the next pixel
 * (7/16) and over the three below (3/16, 5/16, 1/16). Transparent pixels are
 * not drawn: they take no error and spread none. Returns the pixels, 16 bits
 * each.
 */
function ditherTo16Bits(width, height, rgba) {
    const pixels = new Uint16Array(width * height);
    const BITS = [5, 6, 5];

    // the errors to add to this row and to the next one, per channel (one
    // more column on each side: no test at the borders)
    let current = [new Float32Array(width + 2), new Float32Array(width + 2), new Float32Array(width + 2)];
    let next = [new Float32Array(width + 2), new Float32Array(width + 2), new Float32Array(width + 2)];

    for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
            const at = (y * width + x) * 4;
            if (rgba[at + 3] === 0) {
                continue;
            }

            // each channel: the closest level, its error spread
            const levels = [0, 0, 0];
            for (let channel = 0; channel < 3; channel++) {
                const bits = BITS[channel];
                const highest = (1 << bits) - 1;
                const wanted = rgba[at + channel] + current[channel][x + 1];
                const level = Math.max(0, Math.min(highest, Math.round((wanted * highest) / 255)));

                // the value the level stands for: its bits repeated, as the
                // display widens them
                const shown = (level << (8 - bits)) | (level >> (2 * bits - 8));
                const error = wanted - shown;
                current[channel][x + 2] += (error * 7) / 16;
                next[channel][x] += (error * 3) / 16;
                next[channel][x + 1] += (error * 5) / 16;
                next[channel][x + 2] += error / 16;
                levels[channel] = level;
            }

            pixels[y * width + x] = (levels[0] << 11) | (levels[1] << 5) | levels[2];
        }

        // the next row's errors become the current ones
        [current, next] = [next, current];
        for (const errors of next) {
            errors.fill(0);
        }
    }

    return pixels;
}

/**
 * The runs of one row, appended to 'out' (an array of bytes).
 */
function encodeRow(pixels, alphas, out) {
    const width = pixels.length;
    let x = 0;

    // how many pixels from 'from' are 'pixel', opaque, at most RUN_MAX
    const repeated = (from, pixel) => {
        let count = 0;
        while (from + count < width && count < RUN_MAX && alphas[from + count] === 255 && pixels[from + count] === pixel) {
            count++;
        }
        return count;
    };

    while (x < width) {
        const alpha = alphas[x];
        let kind = RUN_SKIP;
        let count = 1;

        if (alpha === 0) {
            // transparent pixels
            while (x + count < width && count < RUN_MAX && alphas[x + count] === 0) {
                count++;
            }
        } else if (alpha < 255) {
            // pixels partly transparent
            kind = RUN_BLENDED;
            while (x + count < width && count < RUN_MAX && alphas[x + count] > 0 && alphas[x + count] < 255) {
                count++;
            }
        } else if (repeated(x, pixels[x]) >= REPEAT_MIN) {
            // one opaque pixel repeated
            kind = RUN_REPEAT;
            count = repeated(x, pixels[x]);
        } else {
            // opaque pixels that change, until a repeat worth a run starts
            kind = RUN_OPAQUE;
            while (x + count < width && count < RUN_MAX && alphas[x + count] === 255
                && repeated(x + count, pixels[x + count]) < REPEAT_MIN) {
                count++;
            }
        }

        // the run: its kind and length, then what it carries
        out.push((kind << 6) | (count - 1));
        if (kind === RUN_REPEAT) {
            out.push(pixels[x] >> 8, pixels[x] & 0xff);
        } else if (kind === RUN_OPAQUE) {
            for (let index = x; index < x + count; index++) {
                out.push(pixels[index] >> 8, pixels[index] & 0xff);
            }
        } else if (kind === RUN_BLENDED) {
            for (let index = x; index < x + count; index++) {
                out.push(pixels[index] >> 8, pixels[index] & 0xff, alphas[index]);
            }
        }

        x += count;
    }
}

/**
 * The image in the format of the renderer (see common/draw/image.h): its
 * header, the offset of each row, then the runs of the rows.
 */
export function encodeImage(width, height, rgba) {
    const pixels = ditherTo16Bits(width, height, rgba);

    // the alpha of each pixel, row by row
    const alphas = new Uint8Array(width * height);
    for (let index = 0; index < alphas.length; index++) {
        alphas[index] = rgba[index * 4 + 3];
    }

    // the runs of each row, and where each row starts
    const runs = [];
    const rowStarts = [];
    const dataStart = IMAGE_HEADER_BYTES + height * ROW_OFFSET_BYTES;

    for (let y = 0; y < height; y++) {
        rowStarts.push(dataStart + runs.length);
        encodeRow(pixels.subarray(y * width, (y + 1) * width), alphas.subarray(y * width, (y + 1) * width), runs);
    }

    // the file: header (little endian), the row offsets, the runs
    const image = new Uint8Array(dataStart + runs.length);
    const view = new DataView(image.buffer);
    image.set(IMAGE_MAGIC, 0);
    image[4] = IMAGE_VERSION;
    view.setUint16(6, width, true);
    view.setUint16(8, height, true);
    rowStarts.forEach((start, row) => view.setUint32(IMAGE_HEADER_BYTES + row * ROW_OFFSET_BYTES, start, true));
    image.set(runs, dataStart);
    return image;
}

/**
 * A PNG file converted for the renderer of the displays. Throws an Error
 * that says why it cannot (the caller adds which file).
 */
export function imageFromPng(bytes) {
    const { width, height, rgba } = decodePng(bytes);
    return encodeImage(width, height, rgba);
}
