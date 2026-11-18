/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tests of the images of the displays: the PNG decoder of the tool
// (host/js/image.js) against the pixels Node computed for each fixture
// (tests/fixtures/images/make-fixtures.mjs), the files it refuses, the
// format it writes, then that format drawn by the renderer
// (common/draw/image.cpp, draw.cpp, through the module draw_test):
// transparency, edges blended, runs cut by the target, rows of a band. And
// the import { type: "image" } in a package. tests/run-tests.sh also runs
// the fixture tests/fixtures/imageimport from its sources and its package.
// Run with: bin/Debug/qjsx-tests run -A tests/image.test.js
import * as fs from "sys/fs";
import { join } from "stdlib/path";
import { sha256 } from "sys/crypto";
import { decodePng, encodeImage, imageFromPng, inflate } from "qjsx:image";
import { build } from "qjsx:build";
import { readZip } from "qjsx:zip";
import { hex } from "qjsx:util";
import { renderList } from "draw_test";
import { check, equal, throws, report } from "./assert.js";

const FIXTURES = "tests/fixtures/images";
const IMPORT_FIXTURE = "tests/fixtures/imageimport";

// The command of an image in a list of sys/draw (draw.h KIND_IMAGE).
const KIND_IMAGE = 5;

// True if the two arrays hold the same bytes.
function sameBytes(a, b) {
    return a.length === b.length && a.every((byte, index) => byte === b[index]);
}

// The bytes of a fixture.
function fixture(name) {
    return fs.readFileSync(join(FIXTURES, name));
}

// The 16 bits of a pixel of 0xRRGGBB (exact for the colors used here).
function pixelOf(color) {
    return ((color >> 8) & 0xf800) | ((color >> 5) & 0x07e0) | ((color >> 3) & 0x001f);
}

// An RGBA image of 'width' x 'height' from a function (x, y) -> [r, g, b, a].
function rgbaImage(width, height, pixel) {
    const rgba = new Uint8Array(width * height * 4);
    for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
            rgba.set(pixel(x, y), (y * width + x) * 4);
        }
    }
    return rgba;
}

// The pixels of a target 'width' x 'height' with one image drawn at (x, y),
// the target placed at (originX, originY) in the drawing.
function drawn(image, x, y, width, height, background, originX = 0, originY = 0) {
    const items = new Int16Array([KIND_IMAGE, 0, x, y, 0, 0]);
    const bytes = renderList(items, [image], width, height, background, originX, originY);
    const pixels = new Uint16Array(width * height);
    for (let index = 0; index < pixels.length; index++) {
        pixels[index] = (bytes[index * 2] << 8) | bytes[index * 2 + 1];
    }
    return pixels;
}

// 'foreground' over 'background' (16-bit pixels) with an alpha of 8 bits, as
// the renderer blends (draw.cpp blend(), coverage = alpha + alpha / 128).
function blended(background, foreground, alpha) {
    const coverage = alpha + (alpha >> 7);
    const channel = (shift, mask) => {
        const under = (background >> shift) & mask;
        const over = (foreground >> shift) & mask;
        return (under + (((over - under) * coverage) >> 8)) << shift;
    };
    return channel(11, 0x1f) | channel(5, 0x3f) | channel(0, 0x1f);
}

// --- the decoder: each color type and depth, filters, kinds of blocks
const DECODED = [
    "grey1", "grey2", "grey4", "grey8", "grey16", "rgb8", "rgb16",
    "palette1", "palette2", "palette4", "palette8",
    "greyalpha8", "greyalpha16", "rgba8", "rgba16", "stored", "fixed",
];
for (const name of DECODED) {
    const image = decodePng(fixture(`${name}.png`));
    equal(`${image.width} x ${image.height}`, "13 x 7", `${name}: its size`);
    check(sameBytes(image.rgba, fixture(`${name}.rgba`)), `${name}: every pixel as Node computed it`);
}

// --- what it refuses, saying why
throws(() => decodePng(fixture("interlaced.png")), /interlaced/, "an interlaced image is refused");
throws(() => decodePng(fixture("damaged.png")), /CRC/, "a damaged chunk is seen");
throws(() => decodePng(fixture("cut.png")), /ends too early/, "a file cut short is seen");
throws(() => decodePng(new TextEncoder().encode("GIF89a, not a PNG")), /not a PNG/, "another format is refused");
throws(() => inflate(new Uint8Array([0x78, 0x9c, 0x07]), 10), /no such block type|ends too early/, "bad deflate data is seen");

// --- the format: header, offsets of the rows, the four kinds of runs
{
    // one row: 2 transparent, 70 red (a repeat of 64, then 6 more: a repeat),
    // 2 different opaque, 1 half transparent
    const RED = [255, 0, 0, 255];
    const row = [[0, 0, 0, 0], [0, 0, 0, 0], ...Array(70).fill(RED), [0, 255, 0, 255], [0, 0, 255, 255], [255, 255, 255, 128]];
    const rgba = rgbaImage(row.length, 2, x => row[x]);
    const image = encodeImage(row.length, 2, rgba);
    const view = new DataView(image.buffer, image.byteOffset, image.byteLength);

    equal(String.fromCharCode(...image.subarray(0, 4)), "QIMG", "an image starts with QIMG");
    equal(image[4], 1, "version 1");
    equal(view.getUint16(6, true), 75, "its width");
    equal(view.getUint16(8, true), 2, "its height");
    equal(view.getUint32(12, true), 20, "the first row starts after the table of the rows");

    // the runs of the first row: kind << 6 | length - 1, then what they carry
    const runs = Array.from(image.subarray(view.getUint32(12, true), view.getUint32(16, true)));
    const expected = [
        (0 << 6) | 1,                                    // skip 2
        (3 << 6) | 63, 0xf8, 0x00,                       // repeat red 64 times
        (3 << 6) | 5, 0xf8, 0x00,                        // and 6 more
        (1 << 6) | 1, 0x07, 0xe0, 0x00, 0x1f,            // green, blue
        (2 << 6) | 0, 0xff, 0xff, 128,                   // white at half
    ];
    equal(runs.join(), expected.join(), "the runs of a row: skip, repeat, opaque, blended");
    check(sameBytes(image.subarray(view.getUint32(16, true)), new Uint8Array(runs)), "the second row, the same");
}

// --- dithering: a flat grey that 16 bits cannot show is shown on average
{
    const GREY = 100;
    const rgba = rgbaImage(64, 16, () => [GREY, GREY, GREY, 255]);
    const pixels = drawn(encodeImage(64, 16, rgba), 0, 0, 64, 16, 0);
    let red = 0;
    let green = 0;
    let levels = new Set();
    for (const pixel of pixels) {
        const redLevel = pixel >> 11;
        const greenLevel = (pixel >> 5) & 0x3f;
        red += (redLevel << 3) | (redLevel >> 2);
        green += (greenLevel << 2) | (greenLevel >> 4);
        levels.add(redLevel);
    }
    check(levels.size >= 2, "the grey is made of two red levels (dithered)");
    check(Math.abs(red / pixels.length - GREY) < 0.5, `the mean red is the grey (${(red / pixels.length).toFixed(2)})`);
    check(Math.abs(green / pixels.length - GREY) < 0.5, `the mean green too (${(green / pixels.length).toFixed(2)})`);
}

// --- drawn: opaque pixels, transparent ones, edges blended over the target
{
    // a 6 x 4 image: a frame of transparent pixels around blue, the left
    // column of blue half transparent
    const BLUE = 0x0000ff;
    const image = encodeImage(6, 4, rgbaImage(6, 4, (x, y) => {
        const isBorder = x === 0 || x === 5 || y === 0 || y === 3;
        if (isBorder) {
            return [0, 0, 0, 0];
        }
        return [0, 0, 255, x === 1 ? 128 : 255];
    }));
    const BACKGROUND = pixelOf(0x808080);
    const pixels = drawn(image, 2, 1, 10, 6, BACKGROUND);
    const at = (x, y) => pixels[y * 10 + x];

    equal(at(0, 0), BACKGROUND, "outside the image: the background");
    equal(at(2, 1), BACKGROUND, "a transparent pixel of the image: the background");
    equal(at(4, 2), pixelOf(BLUE), "an opaque pixel: its color");
    equal(at(3, 2), blended(BACKGROUND, pixelOf(BLUE), 128), "a half transparent one: blended over the background");
    equal(at(7, 3), BACKGROUND, "the transparent right column");

    // cut on the left and at the top: the image starts outside the target
    const cut = drawn(image, -3, -2, 4, 3, BACKGROUND);
    equal(cut[0], pixelOf(BLUE), "cut on the left and at the top: pixel (3, 2) of the image at (0, 0)");
    equal(cut[1], pixelOf(BLUE), "pixel (4, 2) next to it");
    equal(cut[2], BACKGROUND, "then its transparent column");

    // a band of a larger drawing: the target placed at row 3
    const band = drawn(image, 2, 1, 10, 2, BACKGROUND, 0, 3);
    equal(band[4], pixelOf(BLUE), "in a band starting at row 3: row 3 of the drawing is row 2 of the image");
    equal(band[10 + 4], BACKGROUND, "and row 4 of the drawing its transparent last row");

    // cut on the right: a run goes past the target
    const right = drawn(image, 6, 1, 8, 4, BACKGROUND);
    equal(right[2 * 8 + 7], blended(BACKGROUND, pixelOf(BLUE), 128), "cut on the right: the half transparent column only");
    equal(right[2 * 8 + 6], BACKGROUND, "after its transparent column");
}

// --- a long image cut through a repeat: only the pixels inside are drawn
{
    const RED = 0xff0000;
    const image = encodeImage(100, 1, rgbaImage(100, 1, () => [255, 0, 0, 255]));
    const pixels = drawn(image, -50, 0, 20, 1, 0);
    check(pixels.every(pixel => pixel === pixelOf(RED)), "a run cut on both sides fills the target exactly");
    const shifted = drawn(image, 15, 0, 20, 1, 0);
    check(shifted.slice(0, 15).every(pixel => pixel === 0) && shifted.slice(15).every(pixel => pixel === pixelOf(RED)), "and starts where it is placed");
}

// --- a damaged image: no crash, the rows out of the file are not drawn
{
    const image = imageFromPng(fixture("rgba8.png"));
    const damaged = image.slice(0, image.length - 10);
    drawn(damaged, 0, 0, 13, 7, 0);
    check(true, "a cut image is drawn without reading past its end");
    throws(() => renderList(new Int16Array(6), [new Uint8Array(8)], 4, 4, 0, 0, 0), /not an image/, "bytes that are not an image are refused");
}

// --- the import { type: "image" }: the package holds the converted image
{
    const built = build(IMPORT_FIXTURE);
    const entries = readZip(fs.readFileSync(built.file));
    const converted = imageFromPng(fs.readFileSync(join(IMPORT_FIXTURE, "src/logo.png")));
    const entry = entries.get("bytes/src/logo.png");
    check(entry !== undefined && sameBytes(entry, converted), "the package holds the image converted, under bytes/<module>");
    const manifest = JSON.parse(new TextDecoder().decode(entries.get("manifest.json")));
    const record = manifest.bytes.find(item => item.path === "src/logo.png");
    equal(record?.hash, hex(sha256(converted)), "the manifest gives the hash of the converted image");
}

// --- errors of the import, with the place of the import
{
    const SCRATCH = "build/image-test";
    const writeProject = (name, main, files = {}) => {
        const directory = join(SCRATCH, name);
        fs.rmSync(directory, { recursive: true, force: true });
        fs.mkdirSync(join(directory, "src"), { recursive: true });
        fs.writeFileSync(join(directory, "qjsx.json"), JSON.stringify({ name, main: "src/main.js", sources: ["src/**/*.js"], keep: 1 }));
        fs.writeFileSync(join(directory, "src/main.js"), main);
        for (const [path, data] of Object.entries(files)) {
            fs.writeFileSync(join(directory, path), data);
        }
        return directory;
    };

    const notPng = writeProject("notpng", 'import x from "./x.png" with { type: "image" };\nconsole.log(x);\n', { "src/x.png": "not an image" });
    throws(() => build(notPng), /main\.js:1:\d+: the image 'src\/x\.png' cannot be converted: not a PNG/, "a file that is not a PNG: where, which, why");

    const twoWays = writeProject("twoways", 'import a from "./x.png" with { type: "image" };\nimport b from "./x.png" with { type: "bytes" };\nconsole.log(a, b);\n',
        { "src/x.png": fixture("rgba8.png") });
    throws(() => build(twoWays), /imported both as image and as bytes/, "one file imported as an image and as bytes");

    const unknown = writeProject("unknowntype", 'import a from "./x.png" with { type: "picture" };\nconsole.log(a);\n', { "src/x.png": fixture("rgba8.png") });
    throws(() => build(unknown), /unsupported import type 'picture' \(known: "bytes" or "image"\)/, "an unknown type");
}

report("image");
