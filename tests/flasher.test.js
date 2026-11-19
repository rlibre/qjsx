/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The flasher of the ESP32 family (flashers/esp32.js), without a board: it is
// given a port played here, FakeRom, which behaves as the serial loader of
// the ROM of a chip as Espressif documents it (SLIP frames, the commands the
// flasher uses, the DTR / RTS lines that reset the chip into the loader).
//
//   qjsx run -A tests/flasher.test.js
import { flash, md5 } from "../flashers/esp32.js";
import { check, equal, rejects, report } from "./assert.js";

// ### a chip behind a port ###################################################

const SLIP_END = 0xc0;
const SLIP_ESCAPE = 0xdb;
const FLASH_BYTES = 4 * 1024 * 1024;       // the flash of the fake chip
const START_BAUD_RATE = 115200;

// The commands the fake answers.
const FLASH_BEGIN = 0x02;
const FLASH_DATA = 0x03;
const SYNC = 0x08;
const SPI_SET_PARAMS = 0x0b;
const SPI_ATTACH = 0x0d;
const CHANGE_BAUDRATE = 0x0f;
const SPI_FLASH_MD5 = 0x13;
const GET_SECURITY_INFO = 0x14;

// Error bytes of its answers.
const BAD_FORMAT = 0x05;
const BAD_CHECKSUM = 0x07;

/**
 * A chip with the loader of its ROM, behind the methods of a serial port.
 * Options: chipId (null: an original ESP32, which has no GET_SECURITY_INFO),
 * usbOnly (the reset of a USB-serial bridge does not reach the loader),
 * corrupt (a byte written wrong), dead (it never answers).
 */
class FakeRom {
    constructor({ chipId = null, usbOnly = false, corrupt = false, dead = false } = {}) {
        this.chipId = chipId;
        this.usbOnly = usbOnly;
        this.corrupt = corrupt;
        this.dead = dead;
        this.flash = new Uint8Array(FLASH_BYTES).fill(0xff);
        this.lines = { dtr: false, rts: false };   // the lines of the port
        this.bootLatched = false;                  // the boot pin was low just before the reset
        this.inLoader = false;                     // download mode
        this.running = false;                      // restarted to run the flash
        this.portBaud = START_BAUD_RATE;           // the speed of the port
        this.loaderBaud = START_BAUD_RATE;         // the speed the loader listens at
        this.flashSize = undefined;                // told by SPI_SET_PARAMS
        this.attached = false;                     // SPI_ATTACH came
        this.begins = [];                          // the FLASH_BEGIN commands: their words
        this.writeBaud = 0;                        // the speed of the loader when the last block came
        this.area = null;                          // the area being written: { address, blocks, blockSize, next }
        this.baudChanges = 0;                      // setBaudRate() calls
        this.received = [];                        // bytes of the frame being received
        this.inFrame = false;
        this.escaped = false;
        this.callback = () => {};
    }

    // --- the port

    onData(callback) {
        this.callback = callback;
    }

    setBaudRate(baudRate) {
        this.portBaud = baudRate;
        this.baudChanges++;
    }

    /**
     * The lines of the port: RTS alone holds the chip in reset, DTR alone
     * pulls its boot pin low; the chip enters its loader when its reset is
     * released with the boot pin low (a bridge), or after a reset that
     * followed the boot pin low at once (the USB Serial/JTAG).
     */
    setSignals(lines) {
        const before = { ...this.lines };
        this.lines = { ...this.lines, ...lines };
        const wasReset = before.rts && !before.dtr;
        const wasBootLow = before.dtr && !before.rts;
        const isReset = this.lines.rts && !this.lines.dtr;
        const isBootLow = this.lines.dtr && !this.lines.rts;

        // into reset: remember the boot pin of the moment before
        if (isReset && !wasReset) {
            this.bootLatched = wasBootLow;
        }

        // out of reset: the loader, or the program
        if (wasReset && !isReset) {
            const byBridge = isBootLow && !this.usbOnly;
            this.inLoader = byBridge || this.bootLatched;
            this.running = !this.inLoader;
            this.loaderBaud = START_BAUD_RATE;
            this.area = null;
        }
    }

    /**
     * Bytes written to the port: frames, when the loader listens at the
     * speed of the port.
     */
    write(bytes) {
        const listening = this.inLoader && !this.dead && this.portBaud === this.loaderBaud;
        if (!listening) {
            return;
        }

        for (const byte of bytes) {
            if (byte === SLIP_END) {
                if (this.inFrame && this.received.length > 0) {
                    this.command(new Uint8Array(this.received));
                }
                this.inFrame = true;
                this.received = [];
                this.escaped = false;
            } else if (this.escaped) {
                this.received.push(byte === 0xdc ? SLIP_END : SLIP_ESCAPE);
                this.escaped = false;
            } else if (byte === SLIP_ESCAPE) {
                this.escaped = true;
            } else {
                this.received.push(byte);
            }
        }
    }

    // --- the loader

    /**
     * Answers a command: 'data', then the 4 status bytes; sent later, in two
     * pieces, after a line of text as the chip prints some.
     */
    answer(command, data, error = 0, value = 0) {
        const body = new Uint8Array(data.length + 4);
        body.set(data);
        body[data.length] = error === 0 ? 0 : 1;
        body[data.length + 1] = error;

        const plain = new Uint8Array(8 + body.length);
        const view = new DataView(plain.buffer);
        plain[0] = 0x01;
        plain[1] = command;
        view.setUint16(2, body.length, true);
        view.setUint32(4, value, true);
        plain.set(body, 8);

        const frame = [SLIP_END];
        for (const byte of plain) {
            if (byte === SLIP_END) {
                frame.push(SLIP_ESCAPE, 0xdc);
            } else if (byte === SLIP_ESCAPE) {
                frame.push(SLIP_ESCAPE, 0xdd);
            } else {
                frame.push(byte);
            }
        }
        frame.push(SLIP_END);

        const text = Array.from("boot text\r\n", character => character.charCodeAt(0));
        const half = frame.length >> 1;
        setTimeout(() => this.callback(new Uint8Array([...text, ...frame.slice(0, half)])), 0);
        setTimeout(() => this.callback(new Uint8Array(frame.slice(half))), 1);
    }

    /**
     * A command of the host.
     */
    command(frame) {
        const view = new DataView(frame.buffer);
        const command = frame[1];
        const size = view.getUint16(2, true);
        const checksum = view.getUint32(4, true);
        const data = frame.subarray(8, 8 + size);
        const words = [];
        for (let offset = 0; offset + 4 <= data.length; offset += 4) {
            words.push(view.getUint32(8 + offset, true));
        }

        switch (command) {
            case SYNC: {
                // answered several times, as the ROM does
                this.answer(SYNC, [], 0, 0x20120707);
                this.answer(SYNC, [], 0, 0x20120707);
                break;
            }
            case GET_SECURITY_INFO: {
                if (this.chipId === null) {
                    this.answer(command, [], BAD_FORMAT);
                    break;
                }

                const info = new Uint8Array(20);
                new DataView(info.buffer).setUint32(12, this.chipId, true);
                this.answer(command, info);
                break;
            }
            case SPI_ATTACH: {
                this.attached = size === 8;
                this.answer(command, [], this.attached ? 0 : BAD_FORMAT);
                break;
            }
            case SPI_SET_PARAMS: {
                this.flashSize = words[1];
                this.answer(command, []);
                break;
            }
            case CHANGE_BAUDRATE: {
                this.answer(command, []);
                this.loaderBaud = words[0];
                break;
            }
            case FLASH_BEGIN: {
                // 4 words, 5 for a chip that tells its id
                const expected = this.chipId === null ? 4 : 5;
                if (words.length !== expected || !this.attached) {
                    this.answer(command, [], BAD_FORMAT);
                    break;
                }

                this.begins.push(words);
                this.area = { address: words[3], blocks: words[1], blockSize: words[2], next: 0 };
                this.answer(command, []);
                break;
            }
            case FLASH_DATA: {
                this.writeBlock(command, words, data.subarray(16), checksum);
                break;
            }
            case SPI_FLASH_MD5: {
                const text = md5(this.flash.subarray(words[0], words[0] + words[1]));
                this.answer(command, Array.from(text, character => character.charCodeAt(0)));
                break;
            }
            default: {
                this.answer(command, [], BAD_FORMAT);
                break;
            }
        }
    }

    /**
     * A block of FLASH_DATA: in order, of the size announced, its checksum
     * right; then written.
     */
    writeBlock(command, words, bytes, checksum) {
        const area = this.area;
        const expectedBlock = area !== null && words[1] === area.next && words[0] === area.blockSize && bytes.length === area.blockSize;
        if (!expectedBlock) {
            this.answer(command, [], BAD_FORMAT);
            return;
        }

        let sum = 0xef;
        for (const byte of bytes) {
            sum ^= byte;
        }

        if (sum !== checksum) {
            this.answer(command, [], BAD_CHECKSUM);
            return;
        }

        this.flash.set(bytes, area.address + area.next * area.blockSize);
        if (this.corrupt && area.next === 0) {
            this.flash[area.address] ^= 0x01;
        }

        area.next++;
        this.writeBaud = this.loaderBaud;
        this.answer(command, []);
    }
}

// ### the files ##############################################################

// An image for the chip 'chipId' (0xE9, the id at byte 12), of 'size' bytes
// that hold every value (the bytes SLIP escapes among them).
function image(chipId, size) {
    const bytes = Uint8Array.from({ length: size }, (unused, index) => (index * 7 + 3) & 0xff);
    bytes[0] = 0xe9;
    bytes[12] = chipId & 0xff;
    bytes[13] = chipId >> 8;
    return bytes;
}

// A file that is not an image.
function table(size) {
    return Uint8Array.from({ length: size }, (unused, index) => 0xc0 + (index % 32));
}

function firmwareFor(chipId) {
    return {
        flashSize: 4 * 1024 * 1024,
        files: [
            { name: "bootloader.bin", address: 0x2000, bytes: image(chipId, 3000) },
            { name: "table.bin", address: 0x8000, bytes: table(100) },
        ],
    };
}

// Whether the flash holds the files, then 0xFF up to the end of their last block.
function holds(rom, firmware) {
    return firmware.files.every(file => {
        const written = rom.flash.subarray(file.address, file.address + file.bytes.length);
        const same = written.every((byte, index) => byte === file.bytes[index]);
        const blockEnd = file.address + Math.ceil(file.bytes.length / 1024) * 1024;
        const filled = rom.flash.subarray(file.address + file.bytes.length, blockEnd).every(byte => byte === 0xff);
        return same && filled;
    });
}

// ### MD5 ####################################################################

const text = value => new TextEncoder().encode(value);
equal(md5(text("")), "d41d8cd98f00b204e9800998ecf8427e", "md5 of nothing");
equal(md5(text("abc")), "900150983cd24fb0d6963f7d28e17f72", "md5 of \"abc\"");
equal(md5(text("1234567890".repeat(8))), "57edf4a22be3c955ac49da2e2107b67a", "md5 of 80 digits (two blocks)");

// ### a chip that tells its id, behind a USB-serial bridge ###################

// its id holds the two bytes SLIP escapes
const CHIP = 0xdbc0;
{
    const rom = new FakeRom({ chipId: CHIP });
    const firmware = firmwareFor(CHIP);
    const lines = [];
    await flash({ port: rom, firmware, log: line => lines.push(line) });

    check(lines[0].includes("USB-serial bridge"), `in download mode by the bridge's reset (${lines[0]})`);
    check(lines.includes(`chip id: ${CHIP}`), "the chip id is read (escaped bytes in the answer)");
    check(holds(rom, firmware), "the flash holds the two files, their last block filled with 0xFF");
    equal(rom.flashSize, 4 * 1024 * 1024, "the size of the flash is told to the loader");
    equal(rom.begins.length, 2, "one FLASH_BEGIN per file");
    equal(rom.begins[0].join(","), `3000,3,1024,${0x2000},0`, "FLASH_BEGIN: size, blocks, block size, address, the fifth word");
    equal(rom.portBaud, 921600, "the port is at the speed of the transfer");
    equal(rom.writeBaud, 921600, "... as the loader was while the blocks came");
    check(rom.running && !rom.inLoader, "the chip is restarted to run");
    check(lines.some(line => line.includes("written and checked")), "each file is checked");
}

// ### the USB Serial/JTAG of a chip ##########################################

{
    const rom = new FakeRom({ chipId: CHIP, usbOnly: true });
    const firmware = firmwareFor(CHIP);
    const lines = [];
    await flash({ port: rom, firmware, baudRate: 115200, log: line => lines.push(line) });

    check(lines[0].includes("USB Serial/JTAG"), `the second way in is tried (${lines[0]})`);
    check(holds(rom, firmware), "the flash holds the files");
    equal(rom.baudChanges, 0, "115200 bauds: the speed is not changed");
    check(rom.running, "the chip is restarted to run");
}

// ### an original ESP32 ######################################################

{
    const rom = new FakeRom({ chipId: null });
    const firmware = firmwareFor(0);
    const lines = [];
    await flash({ port: rom, firmware, log: line => lines.push(line) });

    check(lines.some(line => line.startsWith("chip: ESP32")), "a loader without GET_SECURITY_INFO is an ESP32");
    equal(rom.begins[0].length, 4, "FLASH_BEGIN has four words there");
    check(holds(rom, firmware), "the flash holds the files");
}

// ### what is refused ########################################################

{
    const rom = new FakeRom({ chipId: CHIP });
    await rejects(flash({ port: rom, firmware: firmwareFor(9) }), /made for the chip 9/, "an image of another chip is refused");
    equal(rom.begins.length, 0, "... before anything is erased");
}

{
    const rom = new FakeRom({ chipId: null });
    await rejects(flash({ port: rom, firmware: firmwareFor(9) }), /made for the chip 9/, "an image of another chip is refused by an ESP32");
}

{
    const rom = new FakeRom({ chipId: CHIP, corrupt: true });
    await rejects(flash({ port: rom, firmware: firmwareFor(CHIP) }), /does not hold what was written/, "a flash that holds something else is reported");
}

{
    const rom = new FakeRom({ dead: true });
    const start = Date.now();
    await rejects(flash({ port: rom, firmware: firmwareFor(0) }), /does not answer in download mode/, "a chip that never answers is reported");
    check(Date.now() - start < 15000, "... within a few seconds");
}

report("flasher");
