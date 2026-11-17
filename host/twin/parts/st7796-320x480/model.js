/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A color display of 320 x 480 pixels driven by an ST7796 controller on
// SPI (the screen of the WT32-SC01), with its data / command pin, its reset
// pin and its backlight:
//
//   { "id": "screen", "model": "st7796-320x480", "label": "Screen",
//     "spi": { "port": 2, "cs": 15 }, "dc": 21, "reset": 22, "backlight": 23 }
//
// "dc", "reset" and "backlight" are optional. The level of the DC pin is
// read from each segment of a transaction (device/spi drives it when the
// program's device has its "dc"): 0 a command, 1 its parameters or pixels.
//
// The commands understood (MIPI DCS, as the ST7796 has them): SWRESET 0x01,
// SLPIN 0x10, SLPOUT 0x11, INVOFF 0x20, INVON 0x21, DISPOFF 0x28, DISPON
// 0x29, CASET 0x2A, RASET 0x2B, RAMWR 0x2C, MADCTL 0x36, COLMOD 0x3A, and
// RAMWRC 0x3C (memory write continue); the others are ignored with their
// parameters. Pixels: 16 bits (COLMOD 0x55, high byte first) or 18 bits
// (0x66, three bytes); RAMWR fills the window of CASET / RASET row by row
// from its top left, and starts again at its top left after its last pixel.
//
// MADCTL (memory access): MY 0x80 mirrors the rows, MX 0x40 the columns, MV
// 0x20 exchanges rows and columns, BGR 0x08 the order of the colors. The
// memory is kept as the glass is seen with the board held USB connector at
// the bottom, 320 wide, 480 high; with MX alone (0x48, rotation 0 of
// stdlib/display) the program's (0, 0) is its top left and its x goes right.
// So the column order of the panel is reversed: memory column c is glass
// column 319 - c. The address (column c, row r) of the program goes to:
//   MV 0: glass x = MX ? c : 319 - c, glass y = MY ? 479 - r : r
//   MV 1: glass x = MX ? r : 319 - r, glass y = MY ? 479 - c : c
// which gives the four turns of stdlib/display: 0x48 0 degree, 0x28 the
// image turned 90 degrees clockwise, 0x88 180, 0xe8 270. BGR set: the
// colors as sent (the panel's subpixels are in that order); clear: red and
// blue exchanged.
//
// The reset pin at 0 (a falling edge) and SWRESET clear the memory to black
// and put the controller back as at power on: asleep, display off, MADCTL
// 0, 18 bits per pixel (to check against the datasheet), the whole memory as
// window. The backlight's brightness is the duty cycle of the PWM on its pin
// (or its level).
//
// Speed: the pixels of a memory write are taken a row of the window at a
// time (a straight line on the glass: one loop, no call per pixel); a pixel
// that does not change marks nothing. The glass is cut in tiles of TILE x
// TILE pixels; a pixel that changes marks its tile. The memory holds the
// pixels in the byte order of the wire (high byte first), so that the rows
// sent to the page are copied whole.
//
// The page: every FLUSH_MS at most, the dirty tiles as a few rectangles
// (the tiles side by side on a row of tiles, merged with the same ones of
// the row above), their pixels in base64 (16 bits, high byte first, red in
// the high bits), with the look of the screen: { width, height, awake, on,
// inverted, brightness, turn (how the image is turned on the glass: 0, 90,
// 180, 270), rects: [{ x, y, width, height, pixels }] }. Nothing is sent
// when nothing changed. The action "refresh" sends the whole glass (a page
// that opens asks for it).
import { Part, parsePin } from "qjsx:twin";

// The glass, in pixels, as seen USB connector at the bottom.
const WIDTH = 320;
const HEIGHT = 480;

// The tiles of the glass that changed pixels mark.
const TILE = 16;
const TILES_X = WIDTH / TILE;
const TILES_Y = HEIGHT / TILE;

// How often the changes go to the page, in ms (15 times a second at most).
const FLUSH_MS = 66;

// The commands.
const SOFTWARE_RESET = 0x01;
const SLEEP_IN = 0x10;
const SLEEP_OUT = 0x11;
const INVERSION_OFF = 0x20;
const INVERSION_ON = 0x21;
const DISPLAY_OFF = 0x28;
const DISPLAY_ON = 0x29;
const COLUMN_RANGE = 0x2a;
const ROW_RANGE = 0x2b;
const MEMORY_WRITE = 0x2c;
const MEMORY_ACCESS = 0x36;
const PIXEL_FORMAT = 0x3a;
const MEMORY_WRITE_CONTINUE = 0x3c;

// The bits of MEMORY_ACCESS.
const MIRROR_ROWS = 0x80;           // MY
const MIRROR_COLUMNS = 0x40;        // MX
const EXCHANGE = 0x20;              // MV
const BGR = 0x08;                   // the order of the colors

// PIXEL_FORMAT: its low nibble gives the bits of a pixel sent.
const FORMAT_16_BITS = 0x5;
const FORMAT_18_BITS = 0x6;
const FORMAT_AT_RESET = 0x66;

/**
 * The 16 bits of a pixel with its red and blue fields exchanged.
 */
function swapRedBlue(pixel) {
    return ((pixel & 0x001f) << 11) | (pixel & 0x07e0) | (pixel >> 11);
}

/**
 * A pixel in wire order: the number whose two bytes in memory are its high
 * byte, then its low byte (the PC is little endian), so that the memory's
 * bytes are what the page gets, copied a row at a time.
 */
function toWire(pixel) {
    return ((pixel & 0xff) << 8) | (pixel >> 8);
}

/**
 * Whether (x, y) is a pixel of the glass.
 */
function isOnGlass(x, y) {
    return x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT;
}

export default class St7796 extends Part {
    /**
     * The controller on its chip select, its pins taken, as at power on;
     * then the page told of the changes every FLUSH_MS.
     */
    constructor(config, board) {
        super(config, board);

        // the chip select of the SPI bus: every transaction comes here
        board.spi(config.spi ?? {}).attach(config.spi?.cs, this);

        // the data / command pin: noted for the page (its level comes with
        // each segment)
        if (config.dc !== undefined) {
            board.pin(parsePin(config.dc, "dc"));
        }

        // the reset pin: at 0, the controller is reset
        if (config.reset !== undefined) {
            const reset = board.pin(parsePin(config.reset, "reset"));
            reset.onChange(level => {
                if (level === 0) {
                    this._reset();
                }
            });
        }

        // the backlight pin: its PWM gives the brightness
        this._backlight = null;
        if (config.backlight !== undefined) {
            this._backlight = board.pin(parsePin(config.backlight, "backlight"));
        }

        // the memory, the controller as at power on, nothing shown yet
        this._memory = new Uint16Array(WIDTH * HEIGHT);      // the glass, row by row: a pixel each, in wire order (toWire)
        this._memoryBytes = new Uint8Array(this._memory.buffer);   // the same, as the bytes sent to the page
        this._dirty = new Uint8Array(TILES_X * TILES_Y);     // the tiles changed since the last flush: 1
        this._lookSent = "";                                  // the look last sent, as JSON
        this._glassX = 0;                                     // the place _locate() found
        this._glassY = 0;
        this._reset();

        // the changes to the page, FLUSH_MS apart
        setInterval(() => this._flush(), FLUSH_MS);
    }

    /**
     * A transaction of the program on its chip select: the bytes of each
     * segment that sends some, at its DC level. Nothing is read back.
     */
    onTransaction(segments) {
        for (const segment of segments) {
            const sends = segment.kind === "write" || segment.kind === "transfer";
            if (sends) {
                this._receive(segment.bytes, segment.dc);
            }
        }

        return null;
    }

    /**
     * "refresh" from the page: the whole glass is sent again.
     */
    onAction(action) {
        if (action === "refresh") {
            this._dirty.fill(1);
            this._lookSent = "";
        }
    }

    // ### The controller #####################################################

    /**
     * Back as at power on: the memory black, asleep, the display off, the
     * whole memory as window.
     */
    _reset() {
        this._memory.fill(0);
        this._dirty.fill(1);
        this._awake = false;                // SLEEP_OUT received
        this._on = false;                   // DISPLAY_ON received
        this._inverted = false;             // INVERSION_ON received
        this._access = 0;                   // MEMORY_ACCESS
        this._format = FORMAT_AT_RESET;     // PIXEL_FORMAT
        this._command = -1;                 // the last command; its parameters follow
        this._parameters = [];              // the parameters of that command so far
        this._partial = [];                 // bytes of a pixel not complete yet
        this._columns = [0, WIDTH - 1];     // CASET: first and last column of the window
        this._rows = [0, HEIGHT - 1];       // RASET: first and last row
        this._column = 0;                   // where the next pixel goes
        this._row = 0;
    }

    /**
     * Bytes at the DC level 'dc': commands (0), or what follows the last
     * command (1): pixels after a memory write, else its parameters.
     */
    _receive(bytes, dc) {
        // commands, one byte each
        if (dc === 0) {
            for (const code of bytes) {
                this._startCommand(code);
            }

            return;
        }

        // pixels, or parameters
        const isWrite = this._command === MEMORY_WRITE || this._command === MEMORY_WRITE_CONTINUE;
        if (isWrite) {
            this._writePixels(bytes);
            return;
        }

        for (const byte of bytes) {
            this._addParameter(byte);
        }
    }

    /**
     * A command: done at once when it has no parameter, else its
     * parameters awaited.
     */
    _startCommand(code) {
        this._command = code;
        this._parameters = [];

        switch (code) {
            case SOFTWARE_RESET: {
                this._reset();
                break;
            }
            case SLEEP_IN:
            case SLEEP_OUT: {
                this._awake = code === SLEEP_OUT;
                break;
            }
            case INVERSION_OFF:
            case INVERSION_ON: {
                this._inverted = code === INVERSION_ON;
                break;
            }
            case DISPLAY_OFF:
            case DISPLAY_ON: {
                this._on = code === DISPLAY_ON;
                break;
            }
            case MEMORY_WRITE: {
                // from the top left of the window
                this._column = this._columns[0];
                this._row = this._rows[0];
                this._partial = [];
                break;
            }
            default: {
                break;
            }
        }
    }

    /**
     * A parameter of the last command: the command done once it has all of
     * them.
     */
    _addParameter(byte) {
        const parameters = this._parameters;
        parameters.push(byte);

        switch (this._command) {
            case COLUMN_RANGE:
            case ROW_RANGE: {
                // first and last, high byte first
                if (parameters.length === 4) {
                    const range = [(parameters[0] << 8) | parameters[1], (parameters[2] << 8) | parameters[3]];
                    if (this._command === COLUMN_RANGE) {
                        this._columns = range;
                    } else {
                        this._rows = range;
                    }
                }

                break;
            }
            case MEMORY_ACCESS: {
                if (parameters.length === 1) {
                    this._access = byte;
                }

                break;
            }
            case PIXEL_FORMAT: {
                if (parameters.length === 1) {
                    this._format = byte;
                }

                break;
            }
            default: {
                break;
            }
        }
    }

    /**
     * Pixels of a memory write, each at the next place of the window: row
     * by row, from its top left again after its last pixel. A pixel cut
     * between two segments is completed by the next one.
     */
    _writePixels(bytes) {
        // the size of a pixel; another format: its pixels are not shown
        const depth = this._format & 0xf;
        const size = depth === FORMAT_16_BITS ? 2 : depth === FORMAT_18_BITS ? 3 : 0;
        if (size === 0) {
            return;
        }

        // the bytes left of a pixel cut before, then these
        const partial = this._partial;
        let at = 0;
        while (partial.length > 0 && partial.length < size && at < bytes.length) {
            partial.push(bytes[at++]);
        }

        if (partial.length === size) {
            this._putPixel(this._pixelOf(partial, 0, size));
            this._partial = [];
        }

        // the whole pixels: 16 bits a row of the window at a time, 18 bits
        // one by one
        const end = bytes.length - ((bytes.length - at) % size);
        if (size === 2) {
            at = this._writeRows(bytes, at, end);
        }

        for (; at < end; at += size) {
            this._putPixel(this._pixelOf(bytes, at, size));
        }

        // the start of a pixel cut at the end, kept
        for (; at < bytes.length; at++) {
            this._partial.push(bytes[at]);
        }
    }

    /**
     * The 16-bit pixels of 'bytes' from 'at' to 'end', a row of the window
     * at a time; returns where it stopped (at 'end').
     */
    _writeRows(bytes, at, end) {
        while (at < end) {
            // what is left of the row of the window, at most the pixels left
            const rowLeft = this._columns[1] - this._column + 1;
            const pixelsLeft = (end - at) >> 1;
            if (rowLeft < 1) {
                // a window with its columns upside down: one by one
                this._putPixel(this._pixelOf(bytes, at, 2));
                at += 2;
                continue;
            }

            const count = Math.min(rowLeft, pixelsLeft);
            this._writeRun(bytes, at, count);
            at += count * 2;
        }

        return at;
    }

    /**
     * 'count' 16-bit pixels of 'bytes' from 'at' on the current row of the
     * window, from the current column: a straight line on the glass, written
     * in one loop; then the place moves on as after each pixel.
     */
    _writeRun(bytes, at, count) {
        // the glass places of its first and last pixels
        this._locate(this._column, this._row);
        const x0 = this._glassX;
        const y0 = this._glassY;
        this._locate(this._column + count - 1, this._row);
        const x1 = this._glassX;
        const y1 = this._glassY;

        // partly outside the glass (a window larger than the memory): one by one
        if (!isOnGlass(x0, y0) || !isOnGlass(x1, y1)) {
            for (let index = 0; index < count; index++) {
                this._putPixel(this._pixelOf(bytes, at + index * 2, 2));
            }

            return;
        }

        // the pixels along the line, those that change written and marked
        const dx = Math.sign(x1 - x0);
        const dy = Math.sign(y1 - y0);
        const step = dy * WIDTH + dx;
        const swap = (this._access & BGR) === 0;
        const memory = this._memory;
        let offset = y0 * WIDTH + x0;
        let first = -1;
        let last = -1;

        for (let index = 0; index < count; index++) {
            // in wire order: the bytes as sent, exchanged (BGR clear: red and
            // blue exchanged first)
            const sent = (bytes[at + 1] << 8) | bytes[at];
            const pixel = swap ? toWire(swapRedBlue(toWire(sent))) : sent;
            if (memory[offset] !== pixel) {
                memory[offset] = pixel;
                if (first < 0) {
                    first = index;
                }

                last = index;
            }

            at += 2;
            offset += step;
        }

        // the tiles of what changed
        if (first >= 0) {
            this._markTiles(x0 + dx * first, y0 + dy * first, x0 + dx * last, y0 + dy * last);
        }

        // the place after them: the next row of the window when this one is done
        this._column += count - 1;
        this._advance();
    }

    /**
     * The 16 bits of the pixel of 'size' bytes at 'at' of 'bytes', as the
     * glass shows it (red in the high bits).
     */
    _pixelOf(bytes, at, size) {
        // 16 bits as sent, or the top 6 bits of each byte of 18 bits
        let pixel;
        if (size === 2) {
            pixel = (bytes[at] << 8) | bytes[at + 1];
        } else {
            pixel = ((bytes[at] >> 3) << 11) | ((bytes[at + 1] >> 2) << 5) | (bytes[at + 2] >> 3);
        }

        // BGR set: the colors as sent; clear: red and blue exchanged
        return (this._access & BGR) !== 0 ? pixel : swapRedBlue(pixel);
    }

    /**
     * Puts a pixel at the place of the next one, then moves the place on in
     * the window.
     */
    _putPixel(pixel) {
        // on the glass, if the address is in the memory, its tile marked when
        // it changes
        this._locate(this._column, this._row);
        const x = this._glassX;
        const y = this._glassY;
        const offset = y * WIDTH + x;
        const wire = toWire(pixel);
        if (isOnGlass(x, y) && this._memory[offset] !== wire) {
            this._memory[offset] = wire;
            this._markTiles(x, y, x, y);
        }

        this._advance();
    }

    /**
     * Moves the place of the next pixel on: along the row, then the next
     * row, then the top left of the window again.
     */
    _advance() {
        this._column++;
        if (this._column > this._columns[1]) {
            this._column = this._columns[0];
            this._row++;
            if (this._row > this._rows[1]) {
                this._row = this._rows[0];
            }
        }
    }

    /**
     * The place on the glass of column 'column', row 'row' of the program
     * (see the top of the file), into this._glassX and this._glassY (fields:
     * no array made for each run).
     */
    _locate(column, row) {
        const access = this._access;
        const mirrorColumns = (access & MIRROR_COLUMNS) !== 0;
        const mirrorRows = (access & MIRROR_ROWS) !== 0;

        // rows and columns exchanged: the program's columns go down the glass
        if ((access & EXCHANGE) !== 0) {
            this._glassX = mirrorColumns ? row : WIDTH - 1 - row;
            this._glassY = mirrorRows ? HEIGHT - 1 - column : column;
            return;
        }

        this._glassX = mirrorColumns ? column : WIDTH - 1 - column;
        this._glassY = mirrorRows ? HEIGHT - 1 - row : row;
    }

    /**
     * How the program's image is turned on the glass, in degrees clockwise:
     * where its x axis points (right 0, down 90, left 180, up 270).
     */
    _turn() {
        // the glass places of two pixels side by side on a row
        this._locate(0, 0);
        const x0 = this._glassX;
        const y0 = this._glassY;
        this._locate(1, 0);
        const x1 = this._glassX;
        const y1 = this._glassY;
        if (y1 > y0) {
            return 90;
        }

        if (y1 < y0) {
            return 270;
        }

        return x1 > x0 ? 0 : 180;
    }

    // ### The page ###########################################################

    /**
     * Marks the tiles of the glass box from (x0, y0) to (x1, y1), corners
     * included, in any order.
     */
    _markTiles(x0, y0, x1, y1) {
        // the tiles of the box
        const left = Math.floor(Math.min(x0, x1) / TILE);
        const right = Math.floor(Math.max(x0, x1) / TILE);
        const top = Math.floor(Math.min(y0, y1) / TILE);
        const bottom = Math.floor(Math.max(y0, y1) / TILE);

        for (let tileY = top; tileY <= bottom; tileY++) {
            this._dirty.fill(1, tileY * TILES_X + left, tileY * TILES_X + right + 1);
        }
    }

    /**
     * The dirty tiles as rectangles of the glass ({ left, top, right, bottom
     * }, corners included), then none dirty: each run of tiles on a row of
     * tiles, merged with the run of the same columns on the row above.
     */
    _dirtyRectangles() {
        const rectangles = [];
        let above = new Map();      // "first/last" tile columns -> the rectangle of the row of tiles above

        for (let tileY = 0; tileY < TILES_Y; tileY++) {
            const current = new Map();
            let tileX = 0;

            while (tileX < TILES_X) {
                // a run of dirty tiles
                if (this._dirty[tileY * TILES_X + tileX] === 0) {
                    tileX++;
                    continue;
                }

                const first = tileX;
                while (tileX < TILES_X && this._dirty[tileY * TILES_X + tileX] !== 0) {
                    tileX++;
                }

                // under the same run of the row above: that rectangle grows,
                // else a new one
                const key = `${first}/${tileX - 1}`;
                let rectangle = above.get(key);
                if (rectangle !== undefined) {
                    rectangle.bottom = (tileY + 1) * TILE - 1;
                } else {
                    rectangle = { left: first * TILE, top: tileY * TILE, right: tileX * TILE - 1, bottom: (tileY + 1) * TILE - 1 };
                    rectangles.push(rectangle);
                }

                current.set(key, rectangle);
            }

            above = current;
        }

        this._dirty.fill(0);
        return rectangles;
    }

    /**
     * The brightness of the backlight, 0 to 1: the duty cycle of its PWM,
     * else its level; 1 without a backlight pin.
     */
    _brightness() {
        // no backlight pin: always lit
        if (this._backlight === null) {
            return 1;
        }

        // a PWM: its active share (active low: the rest)
        const pwm = this._backlight.pwm;
        if (pwm !== null) {
            return pwm.invert ? 1 - pwm.duty : pwm.duty;
        }

        return this._backlight.level;
    }

    /**
     * Sends the page what changed: the pixels of the dirty tiles, and the
     * look of the screen; nothing when nothing changed.
     */
    _flush() {
        // the look now
        const look = {
            width: WIDTH,
            height: HEIGHT,
            awake: this._awake,
            on: this._on,
            inverted: this._inverted,
            brightness: Math.round(this._brightness() * 1000) / 1000,
            turn: this._turn(),
        };
        const lookText = JSON.stringify(look);

        // nothing changed: nothing sent
        const rectangles = this._dirtyRectangles();
        if (rectangles.length === 0 && lookText === this._lookSent) {
            return;
        }

        // the look, and the pixels changed
        this._lookSent = lookText;
        this.publish({ ...look, rects: rectangles.map(rectangle => this._pixelsOf(rectangle)) });
    }

    /**
     * The pixels of a rectangle of the glass, row by row, 16 bits each, high
     * byte first, in base64.
     */
    _pixelsOf({ left, top, right, bottom }) {
        // the rows, copied as they are (the memory is in wire order)
        const width = right - left + 1;
        const height = bottom - top + 1;
        const rowBytes = width * 2;
        const bytes = new Uint8Array(rowBytes * height);

        for (let row = 0; row < height; row++) {
            const from = ((top + row) * WIDTH + left) * 2;
            bytes.set(this._memoryBytes.subarray(from, from + rowBytes), row * rowBytes);
        }

        return { x: left, y: top, width, height, pixels: bytes.toBase64() };
    }
}
