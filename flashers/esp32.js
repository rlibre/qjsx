/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The flasher of the ESP32 family for "qjsx flash" (host/js/flash.js): writes
// the files of a firmware to the flash of a chip through the serial loader of
// its ROM. An external file of the tool (flashers/, next to the executable),
// with no import: it only uses the port it is given.
//
// WHY OUR OWN FLASHER (user, 2026-11-19): so that whoever gets a release of
// qjsx has NO TOOL TO INSTALL to put its firmware on a board: no Python, no
// esptool, no ESP-IDF. The archive holds the executable, the firmwares and
// this file; "qjsx flash --board <name> --port <port>" does the rest. And an
// external JS file rather than code inside the executable: a flasher can be
// fixed, or one added for another family of chips, without building qjsx.
//
//   import { flash } from ".../flashers/esp32.js";
//   await flash({ port, firmware, baudRate, log });
//     port       an open serial port at 115200 bauds, as Serial of
//                device/serial: write(bytes), onData(callback),
//                setSignals({ dtr, rts }), setBaudRate(baudRate)
//     firmware   { flashSize, files: [{ name, address, bytes }] }: the size of
//                the flash in bytes (undefined: not told to the loader) and
//                the files, each a Uint8Array written at its address
//     baudRate   the speed of the transfer (default 921600; 115200: no change)
//     log        log(text): what it does, a line at a time
//
// What it does, in order:
//   1. puts the chip in download mode with the DTR and RTS lines of the port
//      and synchronizes with the loader (a board behind a USB-serial bridge,
//      then the USB Serial/JTAG of the chip, tried in turn);
//   2. asks the loader which chip it is and refuses an image made for another
//      one (the chip id of the image's header);
//   3. attaches the flash, tells its size, raises the speed;
//   4. writes each file, uncompressed, in blocks, then compares the MD5 the
//      loader computes on the flash with the file's;
//   5. restarts the chip by its RTS line.
// The loader of the ROM only: nothing is loaded into the RAM of the chip
// (esptool's "stub" is not used), no compression.
//
// WHERE THIS COMES FROM. Written from Espressif's documentation of esptool,
// read on 2026-11-19 (version "latest"), and from nothing else: no code of
// esptool (GPL-2.0) nor of esptool-js was read to write it.
//   - the protocol (SLIP frames, layout of the commands and of the answers,
//     status bytes, error codes, checksum, the commands and their fields):
//     https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/serial-protocol.html
//     https://docs.espressif.com/projects/esptool/en/latest/esp32s3/advanced-topics/serial-protocol.html
//     https://docs.espressif.com/projects/esptool/en/latest/esp32p4/advanced-topics/serial-protocol.html
//   - the boot pins, the DTR / RTS reset circuit, the offset of the
//     bootloader of each chip:
//     https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html
//     https://docs.espressif.com/projects/esptool/en/latest/esp32s3/advanced-topics/boot-mode-selection.html
//     https://docs.espressif.com/projects/esptool/en/latest/esp32p4/advanced-topics/boot-mode-selection.html
//   - the reset sequence of a USB-serial bridge ("classic reset"), the reset
//     by RTS, the timeouts (3 s a command, 0.1 s a sync, 40 s per MB erased,
//     8 s per MB of MD5):
//     https://docs.espressif.com/projects/esptool/en/latest/esp32/esptool/configuration-file.html
//     https://docs.espressif.com/projects/esptool/en/latest/esp32/esptool/advanced-options.html
//   - the header of an image (0xE9, the chip id in its extended header):
//     https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/firmware-image-format.html
//   - MD5: RFC 1321.
//
// WHAT THE DOCUMENTATION DOES NOT SAY, settled by trials on boards (see
// doc/architecture.md, "Flashing from the tool"; tried 2026-11-19 on an
// ESP32-P4 through its USB Serial/JTAG and on an original ESP32 behind a
// CP210x bridge; not yet on an ESP32-S3):
//   - the reset sequence of the USB Serial/JTAG of a chip (USB_JTAG_RESET);
//   - the size of a block the loader of the ROM takes (BLOCK_SIZE);
//   - the values of SPI_SET_PARAMS besides the size of the flash;
//   - that the new speed applies once CHANGE_BAUDRATE is answered;
//   - that the chip id of GET_SECURITY_INFO is the one of the image's header.

// ### the protocol ###########################################################

// SLIP: a frame starts and ends with END; inside, END and ESCAPE are escaped.
const SLIP_END = 0xc0;
const SLIP_ESCAPE = 0xdb;
const SLIP_ESCAPED_END = 0xdc;
const SLIP_ESCAPED_ESCAPE = 0xdd;

// The first byte of a frame: who sent it.
const FROM_HOST = 0x00;
const FROM_CHIP = 0x01;

// A frame: direction, command, size of the data (2 bytes), checksum or value
// (4 bytes), then the data.
const HEADER_SIZE = 8;

// The commands used.
const FLASH_BEGIN = 0x02;
const FLASH_DATA = 0x03;
const SYNC = 0x08;
const SPI_SET_PARAMS = 0x0b;
const SPI_ATTACH = 0x0d;
const CHANGE_BAUDRATE = 0x0f;
const SPI_FLASH_MD5 = 0x13;
const GET_SECURITY_INFO = 0x14;

// The data of SYNC: 07 07 12 20, then 32 bytes 0x55 (the loader measures the
// speed on them).
const SYNC_DATA = new Uint8Array([0x07, 0x07, 0x12, 0x20, ...new Array(32).fill(0x55)]);

// The answer of the loader of the ROM ends with 4 status bytes: status (0:
// done), error, 2 reserved. A loader that sends 2 (the stub) is read too.
const ROM_STATUS_BYTES = 4;

// What the error byte of an answer means.
const ROM_ERRORS = {
    0x01: "invalid input parameter",
    0x02: "out of memory in the loader",
    0x03: "send failed",
    0x04: "receive failed",
    0x05: "bad message format",
    0x06: "wrong result",
    0x07: "checksum error",
    0x08: "flash write error",
    0x09: "flash read error",
    0x0a: "flash read length error",
    0x0b: "deflate failed",
    0x0c: "deflate checksum error",
    0x0d: "deflate parameter error",
    0x0e: "invalid RAM binary size",
    0x0f: "invalid RAM binary address",
    0x64: "invalid parameter",
    0x65: "invalid format",
    0x66: "description too long",
    0x67: "bad encoding description",
    0x69: "insufficient storage",
};

// The checksum of the data of a block: every byte XORed, from this value.
const CHECKSUM_SEED = 0xef;

// The header of a block of FLASH_DATA: length, sequence number, 0, 0.
const BLOCK_HEADER_WORDS = 4;

// The answer of GET_SECURITY_INFO: flags (4 bytes), a counter (1), key
// purposes (7), then the chip id (4) and its revision (4).
const SECURITY_CHIP_ID_OFFSET = 12;
const SECURITY_INFO_SIZE = 20;

// An image the ROM boots or the bootloader loads: 0xE9 first, the chip id (2
// bytes) at byte 4 of the extended header that follows the 8 bytes of the
// header. An original ESP32 has the id 0 and no GET_SECURITY_INFO.
const IMAGE_MAGIC = 0xe9;
const IMAGE_CHIP_ID_OFFSET = 12;
const IMAGE_HEADERS_SIZE = 24;
const ESP32_CHIP_ID = 0;

// ### what is not in the documentation (trials) ##############################

// Bytes of a file in one FLASH_DATA command.
const BLOCK_SIZE = 0x400;

// The flash as SPI_SET_PARAMS describes it, besides its size: id, block,
// sector and page sizes, status mask.
const FLASH_ID = 0;
const FLASH_BLOCK_SIZE = 0x10000;
const FLASH_SECTOR_SIZE = 0x1000;
const FLASH_PAGE_SIZE = 0x100;
const FLASH_STATUS_MASK = 0xffff;

// The lines of a port, a step at a time: { dtr, rts } set (true: the line
// active, which pulls its pin low), or { wait } milliseconds.
//
// A board behind a USB-serial bridge: RTS holds the chip in reset, DTR pulls
// its boot pin low (Espressif's "classic reset").
const BRIDGE_RESET = [
    { dtr: false, rts: true },
    { wait: 100 },
    { dtr: true, rts: false },
    { wait: 50 },
    { dtr: false },
];

// The USB Serial/JTAG of a chip plays the same two lines itself: at rest,
// the boot pin, then the reset with the boot pin released in the same step,
// then at rest.
const USB_JTAG_RESET = [
    { dtr: false, rts: false },
    { wait: 100 },
    { dtr: true, rts: false },
    { wait: 100 },
    { dtr: false, rts: true },
    { wait: 100 },
    { dtr: false, rts: false },
];

// The chip restarted to run what was written: reset held a moment.
const RUN_RESET = [
    { dtr: false, rts: true },
    { wait: 200 },
    { rts: false },
];

// The ways into the loader, tried in turn.
const WAYS_IN = [
    { name: "USB-serial bridge", steps: BRIDGE_RESET },
    { name: "USB Serial/JTAG", steps: USB_JTAG_RESET },
];

// ### timings ################################################################

const START_BAUD_RATE = 115200;      // the speed the loader starts at
const DEFAULT_BAUD_RATE = 921600;    // the speed of the transfer
const COMMAND_TIMEOUT = 3000;        // ms, an answer
const SYNC_TIMEOUT = 100;            // ms, the answer to a SYNC
const SYNC_ATTEMPTS = 7;             // SYNCs sent after each reset
const ENTRY_ROUNDS = 3;              // times every way in is tried
const AFTER_SYNC_MS = 100;           // the loader answers a SYNC several times: let them come
const AFTER_SPEED_MS = 50;           // the loader settles at its new speed
const SECURITY_INFO_TIMEOUT = 500;   // ms: an original ESP32 does not answer it
const ERASE_MS_PER_MB = 40000;       // FLASH_BEGIN erases the area before answering
const MD5_MS_PER_MB = 8000;          // the loader reads the area back
const MEGABYTE = 1024 * 1024;
const PROGRESS_STEP = 0.1;           // a line of log every tenth of a file...
const PROGRESS_MIN_SIZE = 256 * 1024; // ...of this size at least (a small one is written at once)

// ### helpers ################################################################

/**
 * Resolves after 'ms' milliseconds.
 */
function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

/**
 * 32-bit words as bytes, the least significant byte first.
 */
function wordsToBytes(words) {
    const bytes = new Uint8Array(words.length * 4);
    const view = new DataView(bytes.buffer);
    words.forEach((word, index) => view.setUint32(index * 4, word, true));
    return bytes;
}

/**
 * The time a command that works on 'size' bytes may take: 'msPerMb' for each
 * megabyte, never less than an ordinary answer.
 */
function timeoutFor(size, msPerMb) {
    return Math.max(COMMAND_TIMEOUT, Math.ceil((size / MEGABYTE) * msPerMb));
}

/**
 * A number as "0x" and hexadecimal digits.
 */
function hex(value) {
    return `0x${value.toString(16)}`;
}

// ### MD5 (RFC 1321) #########################################################

// How far each of the 64 steps turns its sum to the left.
const MD5_SHIFTS = [
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
];

// The constant of each step: the integer part of 2^32 x |sin(step + 1)|.
const MD5_CONSTANTS = Array.from({ length: 64 }, (unused, step) => Math.floor(Math.abs(Math.sin(step + 1)) * 2 ** 32) >>> 0);

const MD5_BLOCK_SIZE = 64;           // bytes of a block
const MD5_LENGTH_SIZE = 8;           // bytes of the length that ends the padding

/**
 * The MD5 of 'bytes', as 32 hexadecimal digits in lower case.
 */
export function md5(bytes) {
    // the message padded: a 0x80, zeros, then its length in bits (64 bits,
    // least significant first), the whole a multiple of 64 bytes
    const paddedSize = Math.ceil((bytes.length + 1 + MD5_LENGTH_SIZE) / MD5_BLOCK_SIZE) * MD5_BLOCK_SIZE;
    const padded = new Uint8Array(paddedSize);
    padded.set(bytes);
    padded[bytes.length] = 0x80;
    const view = new DataView(padded.buffer);
    view.setUint32(paddedSize - MD5_LENGTH_SIZE, (bytes.length * 8) >>> 0, true);
    view.setUint32(paddedSize - 4, Math.floor(bytes.length / 2 ** 29), true);

    // the four sums, block after block
    let a0 = 0x67452301;
    let b0 = 0xefcdab89;
    let c0 = 0x98badcfe;
    let d0 = 0x10325476;

    for (let offset = 0; offset < paddedSize; offset += MD5_BLOCK_SIZE) {
        let a = a0;
        let b = b0;
        let c = c0;
        let d = d0;

        for (let step = 0; step < 64; step++) {
            // the function and the word of the block of this step
            let mixed = 0;
            let wordIndex = 0;
            if (step < 16) {
                mixed = (b & c) | (~b & d);
                wordIndex = step;
            } else if (step < 32) {
                mixed = (d & b) | (~d & c);
                wordIndex = (5 * step + 1) % 16;
            } else if (step < 48) {
                mixed = b ^ c ^ d;
                wordIndex = (3 * step + 5) % 16;
            } else {
                mixed = c ^ (b | ~d);
                wordIndex = (7 * step) % 16;
            }

            // the sum, turned to the left, added to b; the four move on
            const word = view.getUint32(offset + wordIndex * 4, true);
            const sum = (a + mixed + MD5_CONSTANTS[step] + word) >>> 0;
            const shift = MD5_SHIFTS[step];
            const turned = ((sum << shift) | (sum >>> (32 - shift))) >>> 0;
            a = d;
            d = c;
            c = b;
            b = (b + turned) >>> 0;
        }

        a0 = (a0 + a) >>> 0;
        b0 = (b0 + b) >>> 0;
        c0 = (c0 + c) >>> 0;
        d0 = (d0 + d) >>> 0;
    }

    // the four sums, least significant byte first, as hexadecimal digits
    const digest = wordsToBytes([a0, b0, c0, d0]);
    return Array.from(digest, byte => byte.toString(16).padStart(2, "0")).join("");
}

// ### the loader of the ROM ##################################################

/**
 * The serial loader of the ROM of a chip, behind a port: frames, commands and
 * their answers.
 */
class RomLoader {
    #port;                          // the serial port
    #frame = null;                  // bytes of the frame being received; null: between two frames
    #escaped = false;               // the last byte received was SLIP_ESCAPE
    #answers = [];                  // answers received, not taken yet: { command, value, data }
    #wake = null;                   // resolves the wait of #nextAnswer; null: nobody waits

    /**
     * The loader behind 'port', which gives its bytes to this object from now.
     */
    constructor(port) {
        this.#port = port;
        port.onData(bytes => this.#receive(bytes));
    }

    /**
     * Bytes of the port: the frames among them become answers (what lies
     * between two frames, the text the chip prints, is dropped).
     */
    #receive(bytes) {
        for (const byte of bytes) {
            // the start of a frame, or its end
            if (byte === SLIP_END) {
                if (this.#frame !== null && this.#frame.length > 0) {
                    this.#takeFrame(this.#frame);
                }
                this.#frame = [];
                this.#escaped = false;
                continue;
            }

            // outside a frame
            if (this.#frame === null) {
                continue;
            }

            // inside: an escaped byte, the escape itself, or a plain byte
            if (this.#escaped) {
                this.#frame.push(byte === SLIP_ESCAPED_END ? SLIP_END : SLIP_ESCAPE);
                this.#escaped = false;
            } else if (byte === SLIP_ESCAPE) {
                this.#escaped = true;
            } else {
                this.#frame.push(byte);
            }
        }
    }

    /**
     * A whole frame: kept as an answer when it is one from the chip, whoever
     * waits is woken.
     */
    #takeFrame(frame) {
        // an answer of the chip, with its whole header
        const isAnswer = frame.length >= HEADER_SIZE && frame[0] === FROM_CHIP;
        if (!isAnswer) {
            return;
        }

        // its command, its value, its data
        const bytes = new Uint8Array(frame);
        const view = new DataView(bytes.buffer);
        const size = view.getUint16(2, true);
        this.#answers.push({
            command: bytes[1],
            value: view.getUint32(4, true),
            data: bytes.subarray(HEADER_SIZE, HEADER_SIZE + size),
        });

        this.#wake?.();
    }

    /**
     * The next answer received, or null when none came before 'deadline' (a
     * time of Date.now()).
     */
    async #nextAnswer(deadline) {
        // one waits already
        if (this.#answers.length > 0) {
            return this.#answers.shift();
        }

        // wait for one, or for the deadline
        const left = deadline - Date.now();
        if (left <= 0) {
            return null;
        }

        let timer = null;
        await new Promise(resolve => {
            this.#wake = resolve;
            timer = setTimeout(resolve, left);
        });
        clearTimeout(timer);
        this.#wake = null;
        return this.#answers.shift() ?? null;
    }

    /**
     * Forgets the answers received and the frame in progress.
     */
    #forgetAnswers() {
        this.#answers = [];
        this.#frame = null;
        this.#escaped = false;
    }

    /**
     * Sends a command as a frame: its header, its data, escaped.
     */
    #send(command, data, checksum) {
        // the frame before the escapes
        const plain = new Uint8Array(HEADER_SIZE + data.length);
        const view = new DataView(plain.buffer);
        plain[0] = FROM_HOST;
        plain[1] = command;
        view.setUint16(2, data.length, true);
        view.setUint32(4, checksum, true);
        plain.set(data, HEADER_SIZE);

        // between two END, END and ESCAPE escaped
        const escaped = [SLIP_END];
        for (const byte of plain) {
            if (byte === SLIP_END) {
                escaped.push(SLIP_ESCAPE, SLIP_ESCAPED_END);
            } else if (byte === SLIP_ESCAPE) {
                escaped.push(SLIP_ESCAPE, SLIP_ESCAPED_ESCAPE);
            } else {
                escaped.push(byte);
            }
        }
        escaped.push(SLIP_END);

        this.#port.write(new Uint8Array(escaped));
    }

    /**
     * Sends a command and waits for its answer (the answers to other commands
     * are skipped). Returns { value, data }, the data without its status
     * bytes; throws when the loader refuses the command or does not answer
     * within 'timeout' ms. 'what' names the command in the error.
     */
    async #command(what, command, data, { checksum = 0, timeout = COMMAND_TIMEOUT } = {}) {
        // send it
        this.#send(command, data, checksum);

        // its answer
        const deadline = Date.now() + timeout;
        let answer = null;
        do {
            answer = await this.#nextAnswer(deadline);
        } while (answer !== null && answer.command !== command);

        if (answer === null) {
            throw new Error(`esp32: ${what}: no answer from the loader`);
        }

        // its status bytes, at the end of the data
        const statusSize = Math.min(ROM_STATUS_BYTES, answer.data.length);
        const status = answer.data.subarray(answer.data.length - statusSize);
        const failed = statusSize < 2 || status[0] !== 0;
        if (failed) {
            const code = statusSize < 2 ? 0 : status[1];
            throw new Error(`esp32: ${what}: the loader refused it (${ROM_ERRORS[code] ?? `error ${hex(code)}`})`);
        }

        return { value: answer.value, data: answer.data.subarray(0, answer.data.length - statusSize) };
    }

    /**
     * Plays steps of the lines of the port: { dtr, rts } or { wait }.
     */
    async #playLines(steps) {
        for (const step of steps) {
            if (step.wait !== undefined) {
                await sleep(step.wait);
            } else {
                this.#port.setSignals(step);
            }
        }
    }

    /**
     * Sends SYNC until the loader answers; false when it never did. The
     * loader answers one SYNC several times: they are left to come, then
     * forgotten.
     */
    async #synchronize() {
        for (let attempt = 0; attempt < SYNC_ATTEMPTS; attempt++) {
            // one SYNC, its answer within a moment
            this.#send(SYNC, SYNC_DATA, 0);
            const deadline = Date.now() + SYNC_TIMEOUT;
            let answer = null;
            do {
                answer = await this.#nextAnswer(deadline);
            } while (answer !== null && answer.command !== SYNC);

            // answered: the other answers to it dropped
            if (answer !== null) {
                await sleep(AFTER_SYNC_MS);
                this.#forgetAnswers();
                return true;
            }
        }

        return false;
    }

    /**
     * Puts the chip in download mode and synchronizes with its loader, each
     * way in tried in turn. Returns the name of the way that worked; throws
     * when none did.
     */
    async enter() {
        for (let round = 0; round < ENTRY_ROUNDS; round++) {
            for (const way of WAYS_IN) {
                // the reset into the loader, then the loader itself
                await this.#playLines(way.steps);
                this.#forgetAnswers();
                if (await this.#synchronize()) {
                    return way.name;
                }
            }
        }

        throw new Error("esp32: the chip does not answer in download mode (hold its BOOT button while it resets, then try again)");
    }

    /**
     * The id of the chip, as in the header of the images made for it; null
     * for a chip whose loader does not tell (the original ESP32).
     */
    async chipId() {
        // asked; a loader without the command refuses it, or says nothing
        let info = null;
        try {
            info = await this.#command("reading the chip id", GET_SECURITY_INFO, new Uint8Array(0), { timeout: SECURITY_INFO_TIMEOUT });
        } catch {
            return null;
        }

        // an answer without the chip id (an older format)
        if (info.data.length < SECURITY_INFO_SIZE) {
            return null;
        }

        return new DataView(info.data.buffer, info.data.byteOffset).getUint32(SECURITY_CHIP_ID_OFFSET, true);
    }

    /**
     * Attaches the flash on its default pins, and tells its size when known.
     */
    async attachFlash(flashSize) {
        // the pins of the chip (0), not the legacy mode (0)
        await this.#command("attaching the flash", SPI_ATTACH, wordsToBytes([0, 0]));

        // its size, so that the loader writes beyond the size it assumes
        if (flashSize !== undefined) {
            const description = [FLASH_ID, flashSize, FLASH_BLOCK_SIZE, FLASH_SECTOR_SIZE, FLASH_PAGE_SIZE, FLASH_STATUS_MASK];
            await this.#command("describing the flash", SPI_SET_PARAMS, wordsToBytes(description));
        }
    }

    /**
     * Changes the speed of the loader and of the port.
     */
    async changeBaudRate(baudRate) {
        // the loader (the second word, 0, is for the loader of the ROM), then
        // the port, once the answer came at the speed before
        await this.#command("changing the speed", CHANGE_BAUDRATE, wordsToBytes([baudRate, 0]));
        this.#port.setBaudRate(baudRate);

        // what was received meanwhile is not at a known speed
        await sleep(AFTER_SPEED_MS);
        this.#forgetAnswers();
    }

    /**
     * Writes 'bytes' to the flash at 'address', then checks them. 'extraWord'
     * (true for the chips that tell their id) adds the "encrypted" word of
     * FLASH_BEGIN, 0. 'progress(written)' is called after each block.
     */
    async writeFile(address, bytes, extraWord, progress) {
        // the area erased: its size, the blocks to come, their size, its address
        const blocks = Math.ceil(bytes.length / BLOCK_SIZE);
        const begin = [bytes.length, blocks, BLOCK_SIZE, address];
        if (extraWord) {
            begin.push(0);
        }

        const eraseTimeout = timeoutFor(bytes.length, ERASE_MS_PER_MB);
        await this.#command(`erasing ${hex(address)}`, FLASH_BEGIN, wordsToBytes(begin), { timeout: eraseTimeout });

        // each block: its header, its bytes (the last one filled with 0xFF),
        // the checksum of its bytes
        for (let block = 0; block < blocks; block++) {
            const data = new Uint8Array(BLOCK_HEADER_WORDS * 4 + BLOCK_SIZE).fill(0xff);
            const part = bytes.subarray(block * BLOCK_SIZE, (block + 1) * BLOCK_SIZE);
            data.set(wordsToBytes([BLOCK_SIZE, block, 0, 0]));
            data.set(part, BLOCK_HEADER_WORDS * 4);

            let checksum = CHECKSUM_SEED;
            for (let index = BLOCK_HEADER_WORDS * 4; index < data.length; index++) {
                checksum ^= data[index];
            }

            await this.#command(`writing ${hex(address + block * BLOCK_SIZE)}`, FLASH_DATA, data, { checksum });
            progress(Math.min(bytes.length, (block + 1) * BLOCK_SIZE));
        }

        // what the flash holds now, against the file
        const md5Timeout = timeoutFor(bytes.length, MD5_MS_PER_MB);
        const answer = await this.#command(`checking ${hex(address)}`, SPI_FLASH_MD5, wordsToBytes([address, bytes.length, 0, 0]), { timeout: md5Timeout });
        const written = RomLoader.#md5Text(answer.data);
        if (written !== md5(bytes)) {
            throw new Error(`esp32: the flash at ${hex(address)} does not hold what was written (MD5 ${written})`);
        }
    }

    /**
     * The MD5 of an answer as hexadecimal digits: the loader of the ROM sends
     * them as 32 characters, another loader as 16 bytes.
     */
    static #md5Text(data) {
        if (data.length >= 32) {
            return String.fromCharCode(...data.subarray(0, 32)).toLowerCase();
        }

        return Array.from(data.subarray(0, 16), byte => byte.toString(16).padStart(2, "0")).join("");
    }

    /**
     * Restarts the chip to run what the flash holds.
     */
    async run() {
        await this.#playLines(RUN_RESET);
    }
}

// ### the flasher ############################################################

/**
 * Refuses the images of 'files' made for another chip than the one that
 * answers ('chipId': null for the original ESP32). A file that is not an
 * image (a partition table) is not checked.
 */
function checkImages(files, chipId) {
    const expected = chipId ?? ESP32_CHIP_ID;

    for (const file of files) {
        // an image with its two headers
        const isImage = file.bytes.length >= IMAGE_HEADERS_SIZE && file.bytes[0] === IMAGE_MAGIC;
        if (!isImage) {
            continue;
        }

        // made for this chip
        const made = file.bytes[IMAGE_CHIP_ID_OFFSET] | (file.bytes[IMAGE_CHIP_ID_OFFSET + 1] << 8);
        if (made !== expected) {
            throw new Error(`esp32: ${file.name} is made for the chip ${made}, this one is the chip ${expected}: wrong board?`);
        }
    }
}

/**
 * Writes a firmware to the chip behind 'port' and restarts it (see the top
 * of the file). Throws an Error that says what failed; the chip is then left
 * as it is, maybe in its loader.
 */
export async function flash({ port, firmware, baudRate = DEFAULT_BAUD_RATE, log = () => {} }) {
    // 1. into the loader
    const loader = new RomLoader(port);
    const way = await loader.enter();
    log(`chip in download mode (${way})`);

    // 2. the chip, against the images
    const chipId = await loader.chipId();
    log(chipId === null ? "chip: ESP32 (its loader does not tell its id)" : `chip id: ${chipId}`);
    checkImages(firmware.files, chipId);

    // 3. the flash, the speed
    await loader.attachFlash(firmware.flashSize);
    if (baudRate !== START_BAUD_RATE) {
        await loader.changeBaudRate(baudRate);
        log(`speed: ${baudRate} bauds`);
    }

    // 4. each file, a line of log every tenth of a long one
    const extraWord = chipId !== null;
    for (const file of firmware.files) {
        log(`${file.name}: ${file.bytes.length} bytes at ${hex(file.address)}`);
        const start = Date.now();
        let logged = 0;
        await loader.writeFile(file.address, file.bytes, extraWord, written => {
            const done = written / file.bytes.length;
            const isLong = file.bytes.length >= PROGRESS_MIN_SIZE;
            if (isLong && done - logged >= PROGRESS_STEP && done < 1) {
                logged = done;
                log(`  ${Math.round(done * 100)} %`);
            }
        });
        log(`  written and checked in ${((Date.now() - start) / 1000).toFixed(1)} s`);
    }

    // 5. run it
    await loader.run();
    log("chip restarted");
}
