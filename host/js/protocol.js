/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Messages of the device agent protocol (shared definition with the C++
// agent: common/agent/protocol.h). A message is a CBOR array:
//
//   [kind, id, payload]
//
//   kind     one of KIND below
//   id       request number (> 0) for a request and its answer, 0 for events
//   payload  map with integer keys (KEY below), or null
//
// Requests are answered by RESPONSE (payload: result) or ERROR (payload:
// { MESSAGE, CODE }). A repeated request id gets the same answer again
// without being executed twice (lost answer on a serial link).
import { encode, decode } from "qjsx:cbor";

export const PROTOCOL_VERSION = 1;
export const DEFAULT_MAX_MESSAGE = 65536;   // a device that does not say (HELLO MAX_MESSAGE)
export const MIN_MAX_MESSAGE = 4096;        // smallest maximum a device may choose
export const UPLOAD_CHUNK = 2048;

export const KIND = {
    RESPONSE: 1,
    ERROR: 2,
    // session
    HELLO: 10,          // { PROTOCOL, TOKEN? } -> { PROTOCOL, AGENT, ENGINE, VERSION?, MAX_MESSAGE, NATIVES? }
    INFO: 11,           // {} -> { RUNNING, BUILD_ID, PACKAGE_SIZE, VOLATILE, DATA_USED?, DATA_TOTAL?,
                        //        JS_HEAP_USED?, JS_HEAP_PEAK?, RAM_FREE?, RAM_LARGEST?, RAM_LOWEST?,
                        //        PSRAM_FREE?, PSRAM_LARGEST? }
    // deployment
    UPLOAD_BEGIN: 20,   // { SIZE, SHA256, BUILD_ID, BYTECODE } -> {}; refused if the device reads another bytecode
    UPLOAD_CHUNK: 21,   // { OFFSET, DATA } -> {}
    UPLOAD_END: 22,     // {} -> { BUILD_ID }   (checks size and sha256, installs)
    RUN: 23,            // {} -> {}   (stops the running program, starts the installed package)
    STOP: 24,           // {} -> {}
    RESET: 25,          // {} -> {}   (ESP: restarts the chip; desktop agent: restarts the runtime)
    // events (id 0)
    LOG: 30,            // { STREAM, TEXT }   console output of the program
    EXITED: 31,         // { CODE }           the program ended
    STARTED: 32,        // { BUILD_ID }       a program started

    // debugger requests (run on the JS thread of the device; while the
    // program runs JS code, only PAUSE, SET_STOPS, SET_EXCEPTIONS and
    // DEBUG_DETACH are handled at once, the others wait for the program to
    // be paused or idle)
    DEBUG_ATTACH: 40,   // {} -> {}   debugging on, for this run and the next ones
    DEBUG_DETACH: 41,   // {} -> {}   debugging off, stops removed, resumes if paused
    PAUSE: 42,          // {} -> {}   pauses at the next statement start (PAUSED event)
    RESUME: 43,         // {} -> {}   resumes after PAUSED or SCRIPT_LOADED
    STEP: 44,           // { STEP } -> {}   resumes and pauses again (STEP_KIND)
    SET_STOPS: 45,      // { MODULE, STOP_KIND, DATA } -> { MISSING }
                        //   DATA: for each function: u16 number, u32 count, count x u32
                        //   offsets (big endian); replaces the stops of these functions.
                        //   MISSING: number of functions not found
    SET_EXCEPTIONS: 46, // { MODE } -> {}   EXCEPTION_MODE
    LIST_SCRIPTS: 47,   // {} -> { SCRIPTS: [module name] }
    GET_STACK: 48,      // {} -> { FRAMES: [[id, module, number, pc, argumentCount,
                        //   localCount, closureCount]] }   paused only; module null and
                        //   counts 0 for a native function
    GET_SLOTS: 49,      // { LEVEL, SLOT_KIND, GROUP } -> { VALUES: [value] }
    SET_SLOT: 50,       // { LEVEL, SLOT_KIND, INDEX, VALUE } -> {}
    GET_PROPERTIES: 51, // { HANDLE, GROUP } -> { PROPERTIES: [[name, flags, value,
                        //   getter, setter]], INTERNALS: [[name, value]] }
                        //   own properties only; name: text or a symbol value; value is
                        //   undefined for an accessor, getter / setter undefined otherwise.
                        //   Getters are never called.
    LOAD_FUNCTION: 52,  // { DATA, GROUP } -> { VALUE }   DATA: bytecode of a script
                        //   compiled on the host whose completion value is a function
    CALL: 53,           // { FUNCTION, THIS, ARGUMENTS, GROUP } -> { VALUE } or { EXCEPTION }
    RELEASE: 54,        // { GROUP } or { HANDLE } -> {}
    // debugger events
    PAUSED: 60,         // { REASON, EXCEPTION?, UNCAUGHT?, RETURN_VALUE? }
    RESUMED: 61,        // {}
    SCRIPT_LOADED: 62,  // { MODULE }   a module was loaded and is not run yet: the device
                        //   waits for RESUME (the host sets the stops of the module first)

    // the files of the data folder of the device (qjsx device ls | read | write | rm |
    // mkdir | mv). PATH: relative to the data folder, '/' separators, ".." refused. An
    // error answer carries ERROR_CODE, a Node-like code ("ENOENT", "ENOSYS": no file
    // system on this device...)
    FILE_LIST: 70,        // { PATH } -> { ENTRIES: [[name, size, isDirectory]] }
    FILE_READ: 71,        // { PATH, OFFSET } -> { DATA (FILE_CHUNK bytes at most), SIZE (of the file) }
    FILE_WRITE_BEGIN: 72, // { PATH, SIZE } -> {}   written to "<path>.qjsx-part"
    FILE_WRITE_CHUNK: 73, // { OFFSET, DATA } -> {}
    FILE_WRITE_END: 74,   // {} -> {}   every byte arrived: renamed onto PATH
    FILE_REMOVE: 75,      // { PATH } -> {}   a file, or an empty folder
    FILE_MKDIR: 76,       // { PATH } -> {}
    FILE_RENAME: 77,      // { PATH, TO } -> {}   a file replaces a file; folders never replace
};

// Largest piece of a file read by one FILE_READ (common/agent/protocol.h).
export const FILE_CHUNK = 2048;

export const KEY = {
    PROTOCOL: 1,
    TOKEN: 2,
    AGENT: 3,
    ENGINE: 4,
    MAX_MESSAGE: 5,
    RUNNING: 6,
    BUILD_ID: 7,
    PACKAGE_SIZE: 8,
    SIZE: 9,
    SHA256: 10,
    OFFSET: 11,
    DATA: 12,
    STREAM: 13,
    TEXT: 14,
    CODE: 15,
    MESSAGE: 16,
    ERROR_CODE: 17,
    MODULE: 18,
    STOP_KIND: 19,
    MISSING: 20,
    MODE: 21,
    SCRIPTS: 22,
    FRAMES: 23,
    LEVEL: 24,
    SLOT_KIND: 25,
    GROUP: 26,
    VALUES: 27,
    INDEX: 28,
    VALUE: 29,
    HANDLE: 30,
    PROPERTIES: 31,
    INTERNALS: 32,
    FUNCTION: 33,
    THIS: 34,
    ARGUMENTS: 35,
    EXCEPTION: 36,
    REASON: 37,
    RETURN_VALUE: 38,
    STEP: 39,
    BYTECODE: 40,       // version of the bytecode of a package (the engine only reads its own)
    UNCAUGHT: 41,       // PAUSED on an exception: true if no handler will catch it (prediction)
    VERSION: 43,        // HELLO answer: qjsx version of the firmware (older firmware: absent)
    NATIVES: 42,        // HELLO answer: names of the native modules of the firmware (older firmware: absent)
    VOLATILE: 44,       // UPLOAD_BEGIN: the package goes to RAM (lost at restart); INFO answer: the package is in RAM
    PATH: 45,           // FILE_*: a path in the data folder
    ENTRIES: 46,        // FILE_LIST answer
    TO: 47,             // FILE_RENAME: the new path
    DATA_USED: 48,      // INFO answer: bytes used by the files of the data folder (no file system: absent)
    DATA_TOTAL: 49,     // INFO answer: bytes of the data partition (absent when unknown: the desktop)
    JS_HEAP_USED: 50,   // INFO answer: bytes of the JS heap of the running program (0 when none runs)
    JS_HEAP_PEAK: 51,   // INFO answer: most bytes of JS heap of the current, or last, run
    RAM_FREE: 52,       // INFO answer: free internal RAM (absent when unknown: the desktop)
    RAM_LARGEST: 53,    // INFO answer: largest free block of internal RAM
    RAM_LOWEST: 54,     // INFO answer: lowest free internal RAM since the boot
    PSRAM_FREE: 55,     // INFO answer: free PSRAM (absent without PSRAM)
    PSRAM_LARGEST: 56,  // INFO answer: largest free block of PSRAM
};

export const STREAM = { STDOUT: 1, STDERR: 2 };

export const STEP_KIND = { INTO: 1, OVER: 2, OUT: 3 };
export const STOP_KIND = { BREAKPOINTS: 1, STATEMENTS: 2 };
export const EXCEPTION_MODE = { NONE: 0, UNCAUGHT: 1, ALL: 2 };
export const SLOT_KIND = { ARGUMENT: 1, LOCAL: 2, CLOSURE: 3 };
// Handle groups, like CDP object groups: PAUSE is released on resume.
export const GROUP = { PAUSE: 1, CONSOLE: 2, PERSISTENT: 3 };
export const PAUSE_REASON = { BREAKPOINT: 1, STEP: 2, DEBUGGER: 3, EXCEPTION: 4 };

// JS values: undefined, null, booleans, numbers and strings are plain CBOR
// values; the others are arrays starting with a tag:
//   [OBJECT, handle, kind, className, description, size, location]
//       kind: OBJECT_KIND; size: Map/Set size, typed array length, ArrayBuffer
//       byte length, Array length, else -1; location: [module, number] for a
//       bytecode function, else null
//   [SYMBOL, handle, description]
//   [BIGINT, decimal text]
//   [UNINITIALIZED]          a variable in its temporal dead zone
//   [HANDLE, handle]         host -> device only: an object known by its handle
export const VALUE = { OBJECT: 1, SYMBOL: 2, BIGINT: 3, UNINITIALIZED: 4, HANDLE: 5 };

// Same order as JSDebugObjectKind (engine/qjsx/quickjs-debug.h).
export const OBJECT_KIND = {
    PLAIN: 0, FUNCTION: 1, ARRAY: 2, ERROR: 3, DATE: 4, REGEXP: 5, MAP: 6, SET: 7,
    WEAKMAP: 8, WEAKSET: 9, PROMISE: 10, PROXY: 11, TYPED_ARRAY: 12, ARRAY_BUFFER: 13,
    DATAVIEW: 14, GENERATOR: 15, ITERATOR: 16, WRAPPER: 17, WEAKREF: 18,
};

export const PROPERTY = { WRITABLE: 1, ENUMERABLE: 2, CONFIGURABLE: 4, ACCESSOR: 8 };

// DATA of SET_STOPS: [{ number, offsets: [pc...] }] -> bytes
// (for each function: u16 number, u32 count, count x u32 offsets, big endian).
export function encodeStops(functions) {
    const size = functions.reduce((total, entry) => total + 6 + entry.offsets.length * 4, 0);
    const bytes = new Uint8Array(size);
    const view = new DataView(bytes.buffer);
    let position = 0;
    for (const entry of functions) {
        view.setUint16(position, entry.number);
        view.setUint32(position + 2, entry.offsets.length);
        position += 6;
        for (const offset of entry.offsets) {
            view.setUint32(position, offset);
            position += 4;
        }
    }
    return bytes;
}

// Cuts the stops of 'functions' ([{ number, offsets }]) into groups whose
// encodeStops() takes at most 'maxBytes' bytes, each function whole: the
// agent replaces the stops function by function, so several SET_STOPS do
// what one would. At least one group (empty when 'functions' is). Throws
// when a function alone does not fit.
export function splitStops(functions, maxBytes) {
    const groups = [];
    let group = [];
    let size = 0;
    for (const entry of functions) {
        const entrySize = 6 + entry.offsets.length * 4;
        if (entrySize > maxBytes) {
            throw new Error(`function ${entry.number} has ${entry.offsets.length} stops: `
                + `more than one message of the device holds (${maxBytes} bytes)`);
        }
        if (size + entrySize > maxBytes) {
            groups.push(group);
            group = [];
            size = 0;
        }
        group.push(entry);
        size += entrySize;
    }
    groups.push(group);
    return groups;
}

export function encodeMessage(kind, id, payload) {
    return encode([kind, id, payload ?? null]);
}

// -> { kind, id, payload: Map | null }
export function decodeMessage(bytes) {
    const message = decode(bytes);
    if (!Array.isArray(message) || message.length !== 3) {
        throw new Error("protocol: a message must be [kind, id, payload]");
    }
    const [kind, id, payload] = message;
    return { kind, id, payload };
}

export function kindName(kind) {
    return Object.keys(KIND).find(name => KIND[name] === kind) ?? `kind ${kind}`;
}
