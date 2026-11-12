/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Device agent protocol: constants shared with host/js/protocol.js
// (keep both files identical in meaning). See that file and
// doc/architecture.md for the message layout: CBOR [kind, id, payload].
#pragma once

namespace agent {

const int PROTOCOL_VERSION = 1;
// Largest message the agent accepts (a function compiled by the debugger
// proxy can be several kilobytes).
const int DEFAULT_MAX_MESSAGE = 65536;

enum Kind {
    KIND_RESPONSE = 1,
    KIND_ERROR = 2,
    KIND_HELLO = 10,
    KIND_INFO = 11,
    KIND_UPLOAD_BEGIN = 20,
    KIND_UPLOAD_CHUNK = 21,
    KIND_UPLOAD_END = 22,
    KIND_RUN = 23,
    KIND_STOP = 24,
    KIND_RESET = 25,
    KIND_LOG = 30,
    KIND_EXITED = 31,
    KIND_STARTED = 32,
    // debugger (handled by agent::Debugger on the JS thread)
    KIND_DEBUG_ATTACH = 40,
    KIND_DEBUG_DETACH = 41,
    KIND_PAUSE = 42,
    KIND_RESUME = 43,
    KIND_STEP = 44,
    KIND_SET_STOPS = 45,
    KIND_SET_EXCEPTIONS = 46,
    KIND_LIST_SCRIPTS = 47,
    KIND_GET_STACK = 48,
    KIND_GET_SLOTS = 49,
    KIND_SET_SLOT = 50,
    KIND_GET_PROPERTIES = 51,
    KIND_LOAD_FUNCTION = 52,
    KIND_CALL = 53,
    KIND_RELEASE = 54,
    // debugger events
    KIND_PAUSED = 60,
    KIND_RESUMED = 61,
    KIND_SCRIPT_LOADED = 62,
};

// First and last debugger request kinds.
const int FIRST_DEBUG_KIND = KIND_DEBUG_ATTACH;
const int LAST_DEBUG_KIND = KIND_RELEASE;

enum Key {
    KEY_PROTOCOL = 1,
    KEY_TOKEN = 2,
    KEY_AGENT = 3,
    KEY_ENGINE = 4,
    KEY_MAX_MESSAGE = 5,
    KEY_RUNNING = 6,
    KEY_BUILD_ID = 7,
    KEY_PACKAGE_SIZE = 8,
    KEY_SIZE = 9,
    KEY_SHA256 = 10,
    KEY_OFFSET = 11,
    KEY_DATA = 12,
    KEY_STREAM = 13,
    KEY_TEXT = 14,
    KEY_CODE = 15,
    KEY_MESSAGE = 16,
    KEY_ERROR_CODE = 17,
    KEY_MODULE = 18,
    KEY_STOP_KIND = 19,
    KEY_MISSING = 20,
    KEY_MODE = 21,
    KEY_SCRIPTS = 22,
    KEY_FRAMES = 23,
    KEY_LEVEL = 24,
    KEY_SLOT_KIND = 25,
    KEY_GROUP = 26,
    KEY_VALUES = 27,
    KEY_INDEX = 28,
    KEY_VALUE = 29,
    KEY_HANDLE = 30,
    KEY_PROPERTIES = 31,
    KEY_INTERNALS = 32,
    KEY_FUNCTION = 33,
    KEY_THIS = 34,
    KEY_ARGUMENTS = 35,
    KEY_EXCEPTION = 36,
    KEY_REASON = 37,
    KEY_RETURN_VALUE = 38,
    KEY_STEP = 39,
    KEY_BYTECODE = 40,
    KEY_UNCAUGHT = 41,
    KEY_NATIVES = 42,
    KEY_VERSION = 43,
    KEY_VOLATILE = 44,
};

enum Stream {
    STREAM_STDOUT = 1,
    STREAM_STDERR = 2,
};

enum StepKind { STEP_INTO = 1, STEP_OVER = 2, STEP_OUT = 3 };

enum StopKind { STOPS_BREAKPOINTS = 1, STOPS_STATEMENTS = 2 };

enum ExceptionMode { EXCEPTIONS_NONE = 0, EXCEPTIONS_UNCAUGHT = 1, EXCEPTIONS_ALL = 2 };

enum SlotKind { SLOT_ARGUMENT = 1, SLOT_LOCAL = 2, SLOT_CLOSURE = 3 };

// Handle groups (like CDP object groups): PAUSE is released on resume.
enum Group { GROUP_PAUSE = 1, GROUP_CONSOLE = 2, GROUP_PERSISTENT = 3 };

enum PauseReason {
    REASON_BREAKPOINT = 1,
    REASON_STEP = 2,          // a step or a pause request
    REASON_DEBUGGER = 3,      // 'debugger;' statement
    REASON_EXCEPTION = 4,
};

// Encoding of JS values (see host/js/protocol.js for the full description):
// undefined, null, booleans, numbers and strings are plain CBOR values;
// the others are arrays starting with one of these tags.
enum ValueTag {
    VALUE_OBJECT = 1,          // [1, handle, kind, className, description, size, location]
    VALUE_SYMBOL = 2,          // [2, handle, description]
    VALUE_BIGINT = 3,          // [3, decimal text]
    VALUE_UNINITIALIZED = 4,   // [4]  variable in its temporal dead zone
    VALUE_HANDLE = 5,          // [5, handle]  host -> agent: an object known by its handle
};

// Property flags (GET_PROPERTIES).
enum PropertyFlag {
    PROPERTY_WRITABLE = 1,
    PROPERTY_ENUMERABLE = 2,
    PROPERTY_CONFIGURABLE = 4,
    PROPERTY_ACCESSOR = 8,
};

} // namespace agent
