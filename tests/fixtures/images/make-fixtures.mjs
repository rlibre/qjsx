/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Makes the PNG files of tests/image.test.js, with Node (its own zlib: no
// dependency), and next to each one '<name>.rgba': the pixels the decoder of
// qjsx (host/js/image.js) must give, 4 bytes each, computed here on their
// own. Run again only to change the fixtures:
//
//   node tests/fixtures/images/make-fixtures.mjs
//
// Every color type in every bit depth, every row filter (the rows take the
// five in turn), the three kinds of deflate blocks (stored, fixed and
// dynamic codes), the transparency of tRNS; and a few files the decoder
// must refuse.
import { deflateSync, constants } from "node:zlib";
import { writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const FOLDER = dirname(fileURLToPath(import.meta.url));
const WIDTH = 13;    // odd, so that the bits of a row do not end on a byte
const HEIGHT = 7;

// The same pseudo-random numbers on every run.
let seed = 12345;
function random(limit) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    return seed % limit;
}

// The CRC-32 of PNG.
const CRC_TABLE = Array.from({ length: 256 }, (_, value) => {
    let crc = value;
    for (let bit = 0; bit < 8; bit++) {
        crc = crc & 1 ? 0xedb88320 ^ (crc >>> 1) : crc >>> 1;
    }
    return crc >>> 0;
});

function crc32(bytes) {
    let crc = 0xffffffff;
    for (const byte of bytes) {
        crc = CRC_TABLE[(crc ^ byte) & 0xff] ^ (crc >>> 8);
    }
    return (crc ^ 0xffffffff) >>> 0;
}

// A chunk: length, type, data, CRC.
function chunk(type, data) {
    const out = Buffer.alloc(12 + data.length);
    out.writeUInt32BE(data.length, 0);
    out.write(type, 4, "latin1");
    Buffer.from(data).copy(out, 8);
    out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
    return out;
}

// The rows filtered, each with the filter (row % 5) in front.
function filterRows(raw, rowBytes, pixelBytes) {
    const out = Buffer.alloc(HEIGHT * (rowBytes + 1));
    for (let row = 0; row < HEIGHT; row++) {
        const filter = row % 5;
        out[row * (rowBytes + 1)] = filter;
        for (let index = 0; index < rowBytes; index++) {
            const value = raw[row * rowBytes + index];
            const left = index >= pixelBytes ? raw[row * rowBytes + index - pixelBytes] : 0;
            const above = row > 0 ? raw[(row - 1) * rowBytes + index] : 0;
            const aboveLeft = row > 0 && index >= pixelBytes ? raw[(row - 1) * rowBytes + index - pixelBytes] : 0;
            let predicted = 0;
            if (filter === 1) {
                predicted = left;
            } else if (filter === 2) {
                predicted = above;
            } else if (filter === 3) {
                predicted = (left + above) >> 1;
            } else if (filter === 4) {
                const estimate = left + above - aboveLeft;
                const a = Math.abs(estimate - left);
                const b = Math.abs(estimate - above);
                const c = Math.abs(estimate - aboveLeft);
                predicted = a <= b && a <= c ? left : b <= c ? above : aboveLeft;
            }
            out[row * (rowBytes + 1) + 1 + index] = (value - predicted) & 0xff;
        }
    }
    return out;
}

// Packs samples of 'depth' bits into rows of bytes, the first one highest.
function packRows(samples, perRow, depth) {
    const rowBytes = Math.ceil((perRow * depth) / 8);
    const out = Buffer.alloc(HEIGHT * rowBytes);
    for (let row = 0; row < HEIGHT; row++) {
        for (let index = 0; index < perRow; index++) {
            const value = samples[row * perRow + index];
            if (depth === 16) {
                out[row * rowBytes + index * 2] = value >> 8;
                out[row * rowBytes + index * 2 + 1] = value & 0xff;
            } else {
                const bit = index * depth;
                out[row * rowBytes + (bit >> 3)] |= value << (8 - depth - (bit & 7));
            }
        }
    }
    return { rows: out, rowBytes };
}

// A sample of 'depth' bits as 8 bits, as the decoder must do it.
function to8(value, depth) {
    return depth === 16 ? value >> 8 : Math.round((value * 255) / ((1 << depth) - 1));
}

// Writes '<name>.png' and '<name>.rgba'.
function write(name, { colorType, depth, samples, channels, extra = [], expected, level = 9, strategy, interlace = 0 }) {
    const { rows, rowBytes } = packRows(samples, WIDTH * channels, depth);
    const pixelBytes = Math.max(1, (channels * depth) >> 3);
    const options = { level };
    if (strategy !== undefined) {
        options.strategy = strategy;
    }
    const compressed = deflateSync(filterRows(rows, rowBytes, pixelBytes), options);

    const header = Buffer.alloc(13);
    header.writeUInt32BE(WIDTH, 0);
    header.writeUInt32BE(HEIGHT, 4);
    header[8] = depth;
    header[9] = colorType;
    header[12] = interlace;

    const png = Buffer.concat([
        Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
        chunk("IHDR", header),
        ...extra,
        chunk("IDAT", compressed),
        chunk("IEND", Buffer.alloc(0)),
    ]);
    writeFileSync(join(FOLDER, `${name}.png`), png);
    if (expected !== undefined) {
        writeFileSync(join(FOLDER, `${name}.rgba`), Buffer.from(expected));
    }
    return png;
}

const PIXELS = WIDTH * HEIGHT;

// grey, every depth (8 bits: a transparent value through tRNS)
for (const depth of [1, 2, 4, 8, 16]) {
    const samples = Array.from({ length: PIXELS }, () => random(1 << depth));
    const transparentValue = depth === 8 ? samples[5] : -1;
    const expected = samples.flatMap(value => {
        const grey = to8(value, depth);
        return [grey, grey, grey, value === transparentValue ? 0 : 255];
    });
    const extra = depth === 8 ? [chunk("tRNS", Buffer.from([0, transparentValue]))] : [];
    write(`grey${depth}`, { colorType: 0, depth, samples, channels: 1, extra, expected });
}

// RGB, 8 bits (a transparent color through tRNS) and 16 bits
for (const depth of [8, 16]) {
    const samples = Array.from({ length: PIXELS * 3 }, () => random(1 << depth));
    const transparent = depth === 8 ? samples.slice(9, 12) : null;
    const expected = [];
    for (let pixel = 0; pixel < PIXELS; pixel++) {
        const rgb = samples.slice(pixel * 3, pixel * 3 + 3);
        const isTransparent = transparent !== null && rgb.every((value, channel) => value === transparent[channel]);
        expected.push(...rgb.map(value => to8(value, depth)), isTransparent ? 0 : 255);
    }
    const extra = depth === 8 ? [chunk("tRNS", Buffer.from([0, transparent[0], 0, transparent[1], 0, transparent[2]]))] : [];
    write(`rgb${depth}`, { colorType: 2, depth, samples, channels: 3, extra, expected });
}

// palette, every depth, with the alpha of some entries
for (const depth of [1, 2, 4, 8]) {
    const entries = Math.min(1 << depth, 20);
    const palette = Array.from({ length: entries * 3 }, () => random(256));
    const alphas = Array.from({ length: Math.min(entries, 5) }, () => random(256));
    const samples = Array.from({ length: PIXELS }, () => random(entries));
    const expected = samples.flatMap(entry => [...palette.slice(entry * 3, entry * 3 + 3), entry < alphas.length ? alphas[entry] : 255]);
    const extra = [chunk("PLTE", Buffer.from(palette)), chunk("tRNS", Buffer.from(alphas))];
    write(`palette${depth}`, { colorType: 3, depth, samples, channels: 1, extra, expected });
}

// grey + alpha and RGB + alpha, 8 and 16 bits
for (const depth of [8, 16]) {
    const greyAlpha = Array.from({ length: PIXELS * 2 }, () => random(1 << depth));
    const expectedGrey = [];
    for (let pixel = 0; pixel < PIXELS; pixel++) {
        const grey = to8(greyAlpha[pixel * 2], depth);
        expectedGrey.push(grey, grey, grey, to8(greyAlpha[pixel * 2 + 1], depth));
    }
    write(`greyalpha${depth}`, { colorType: 4, depth, samples: greyAlpha, channels: 2, expected: expectedGrey });

    const rgba = Array.from({ length: PIXELS * 4 }, () => random(1 << depth));
    write(`rgba${depth}`, { colorType: 6, depth, samples: rgba, channels: 4, expected: rgba.map(value => to8(value, depth)) });
}

// the other kinds of deflate blocks, on one RGBA image: stored and fixed
// codes (the random images above have dynamic codes)
{
    const rgba = Array.from({ length: PIXELS * 4 }, (_, index) => (index * 7) & 0xff);
    write("stored", { colorType: 6, depth: 8, samples: rgba, channels: 4, expected: rgba, level: 0 });
    write("fixed", { colorType: 6, depth: 8, samples: rgba, channels: 4, expected: rgba, strategy: constants.Z_FIXED });
}

// the badge of tests/display.test.js (copied into tests/fixtures/twindisplay):
// 8 x 6, its first column transparent, its second white at half, the rest red
{
    const badge = [];
    for (let pixel = 0; pixel < 8 * 6; pixel++) {
        const x = pixel % 8;
        badge.push(...(x === 0 ? [0, 0, 0, 0] : x === 1 ? [255, 255, 255, 128] : [255, 0, 0, 255]));
    }
    const header = Buffer.alloc(13);
    header.writeUInt32BE(8, 0);
    header.writeUInt32BE(6, 4);
    header[8] = 8;
    header[9] = 6;
    const rows = Buffer.alloc(6 * 33);
    for (let row = 0; row < 6; row++) {
        Buffer.from(badge.slice(row * 32, row * 32 + 32)).copy(rows, row * 33 + 1);
    }
    const png = Buffer.concat([
        Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
        chunk("IHDR", header),
        chunk("IDAT", deflateSync(rows)),
        chunk("IEND", Buffer.alloc(0)),
    ]);
    writeFileSync(join(FOLDER, "badge.png"), png);
}

// refused: interlaced, a damaged CRC, cut short
{
    const rgba = Array.from({ length: PIXELS * 4 }, () => random(256));
    write("interlaced", { colorType: 6, depth: 8, samples: rgba, channels: 4, interlace: 1 });

    const damaged = write("damaged", { colorType: 6, depth: 8, samples: rgba, channels: 4 });
    damaged[40] ^= 0xff;   // inside the IDAT data: its CRC no longer matches
    writeFileSync(join(FOLDER, "damaged.png"), damaged);

    const cut = write("cut", { colorType: 6, depth: 8, samples: rgba, channels: 4 });
    writeFileSync(join(FOLDER, "cut.png"), cut.subarray(0, cut.length - 30));
}
