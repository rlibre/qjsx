/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Global objects: console, timers, TextEncoder / TextDecoder, process.
#include "modules.h"
#include "memory.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "jsbind.h"
#include "loop.h"
#include "files.h"
#include "permissions.h"
#include "random.h"
#include "version.h"

#if defined(_WIN32)
#include <windows.h>
#else
extern char **environ;
#endif

namespace {

// ### output (see rt::setOutputSink) ##########################################

rt::OutputSink *outputSink = nullptr;
void *outputSinkOpaque = nullptr;

/**
 * Error writer given to jsbind (uncaught exceptions...): goes to the
 * program's stderr output, so through the sink when one is set.
 */
void writeErrorOutput(const char *text, size_t length) {
    rt::writeOutput(rt::OUTPUT_STDERR, text, length);
}

// Growing text buffer: a console line is sent in one piece.
struct TextBuffer {
    char *data = nullptr;  // malloc'd, not zero-terminated; nullptr: empty
    size_t length = 0;     // bytes used
    size_t capacity = 0;   // bytes allocated

    /**
     * Frees the buffer.
     */
    ~TextBuffer() {
        memory::release(data);
    }

    /**
     * Appends 'size' bytes (no terminating zero is kept).
     */
    void append(const char *text, size_t size) {
        // when they do not fit: double the capacity (starting at 128) until
        // they do
        if (length + size > capacity) {
            size_t grown = capacity == 0 ? 128 : capacity * 2;

            while (grown < length + size) {
                grown *= 2;
            }

            data = (char *)memory::reallocate(data, grown);
            capacity = grown;
        }

        // copy them at the end
        memcpy(data + length, text, size);
        length += size;
    }
};

/**
 * JS_PrintValue writer: appends to the TextBuffer passed as 'opaque'.
 */
void appendToBuffer(void *opaque, const char *text, size_t length) {
    ((TextBuffer *)opaque)->append(text, length);
}

// ### console #################################################################

/**
 * Writes the console arguments as one line on 'stream', formatted like the
 * qjs console.
 */
void writeValues(int stream, js::Args arguments) {
    // values are formatted by JS_PrintValue with its default options
    JSPrintValueOptions options;
    JS_PrintValueSetDefaultOptions(&options);
    TextBuffer line;

    // append each argument, separated by a space: strings as they are, other
    // values formatted
    for (int i = 0; i < arguments.count; i++) {
        if (i > 0) {
            line.append(" ", 1);
        }

        JSValueConst value = arguments[i];
        if (JS_IsString(value)) {
            size_t length = 0;
            const char *text = JS_ToCStringLen(arguments.ctx, &length, value);
            if (text != nullptr) {
                line.append(text, length);
            }

            JS_FreeCString(arguments.ctx, text);
        } else {
            JS_PrintValue(arguments.ctx, &appendToBuffer, &line, value, &options);
        }
    }

    // end the line and write it at once, so that lines from different sources
    // do not get mixed
    line.append("\n", 1);
    rt::writeOutput(stream, line.data, line.length);
}

/**
 * console.log / info / debug(...values): one line on stdout.
 */
void consoleLog(js::Args arguments) {
    writeValues(rt::OUTPUT_STDOUT, arguments);
}

/**
 * console.warn / error(...values): one line on stderr.
 */
void consoleWarn(js::Args arguments) {
    writeValues(rt::OUTPUT_STDERR, arguments);
}

/**
 * process.stdout.write(text): writes the text as it is (no newline added).
 * Always returns true.
 */
bool writeStdout(js::Str text) {
    rt::writeOutput(rt::OUTPUT_STDOUT, text.data(), text.size());
    return true;
}

/**
 * process.stderr.write(text): writes the text as it is (no newline added).
 * Always returns true.
 */
bool writeStderr(js::Str text) {
    rt::writeOutput(rt::OUTPUT_STDERR, text.data(), text.size());
    return true;
}

// ### crypto: random values ###################################################

const size_t MAX_RANDOM_VALUES = 65536;   // bytes per getRandomValues() call (web standard)

/**
 * True when 'array' (a typed array of 'elementSize' bytes per element) holds
 * floats: its class is the one of a new Float16Array / Float32Array /
 * Float64Array of that element size (QuickJS has no public test).
 */
bool isFloatArray(JSContext *ctx, JSValueConst array, size_t elementSize) {
    // the float kind of that element size, if any
    JSTypedArrayEnum kind;
    if (elementSize == 2) {
        kind = JS_TYPED_ARRAY_FLOAT16;
    } else if (elementSize == 4) {
        kind = JS_TYPED_ARRAY_FLOAT32;
    } else if (elementSize == 8) {
        kind = JS_TYPED_ARRAY_FLOAT64;
    } else {
        return false;
    }

    // an empty array of that kind, to compare the classes (JS_NewTypedArray
    // always reads 3 arguments)
    JSValue arguments[3] = { JS_NewInt32(ctx, 0), JS_UNDEFINED, JS_UNDEFINED };
    JSValue sample = JS_NewTypedArray(ctx, 3, arguments, kind);
    bool isFloat = !JS_IsException(sample) && JS_GetClassID(sample) == JS_GetClassID(array);
    JS_FreeValue(ctx, sample);
    return isFloat;
}

/**
 * Throws the error of the web standard for a request too large: an Error
 * named "QuotaExceededError" (no DOMException in qjsx).
 */
void failQuotaExceeded(JSContext *ctx, size_t size) {
    // a plain Error, then its name
    (void)js::fail("crypto.getRandomValues: %lu bytes asked, %lu at most", (unsigned long)size, (unsigned long)MAX_RANDOM_VALUES);
    JSValue error = JS_GetException(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "QuotaExceededError"));
    JS_Throw(ctx, error);
}

/**
 * crypto.getRandomValues(array): fills an integer typed array with random
 * bytes and returns it (web standard). TypeError for anything else (a float
 * array, a DataView, an ArrayBuffer), QuotaExceededError above 65536 bytes.
 */
js::Value getRandomValues(js::Value array) {
    JSContext *ctx = js::context();

    // a typed array: where its bytes are (anything else throws: replaced by
    // our own message)
    size_t offset = 0;
    size_t size = 0;
    size_t elementSize = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, array.get(), &offset, &size, &elementSize);
    if (JS_IsException(buffer)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return js::fail<js::TypeError>("crypto.getRandomValues: the argument must be an integer typed array");
    }

    // integers only, 65536 bytes at most
    if (isFloatArray(ctx, array.get(), elementSize)) {
        JS_FreeValue(ctx, buffer);
        return js::fail<js::TypeError>("crypto.getRandomValues: the argument must be an integer typed array, not a float one");
    }
    if (size > MAX_RANDOM_VALUES) {
        JS_FreeValue(ctx, buffer);
        failQuotaExceeded(ctx, size);
        return js::Value();
    }

    // fill its bytes
    size_t bufferSize = 0;
    uint8_t *bytes = JS_GetArrayBuffer(ctx, &bufferSize, buffer);
    JS_FreeValue(ctx, buffer);
    if (bytes == nullptr) {
        return js::fail<js::TypeError>("crypto.getRandomValues: the array has no memory (detached buffer)");
    }
    rt::randomBytes(bytes + offset, size);
    return array;
}

/**
 * crypto.randomUUID(): a random UUID, version 4, as 36 lowercase characters
 * (web standard).
 */
js::Value randomUUID() {
    // 16 random bytes, then the version (4) and the variant (10xx)
    uint8_t bytes[16];
    rt::randomBytes(bytes, sizeof(bytes));
    bytes[6] = (uint8_t)((bytes[6] & 0x0F) | 0x40);
    bytes[8] = (uint8_t)((bytes[8] & 0x3F) | 0x80);

    // 8-4-4-4-12 hexadecimal digits
    static const char digits[] = "0123456789abcdef";
    char text[37];
    int position = 0;
    for (int index = 0; index < 16; index++) {
        bool dashBefore = index == 4 || index == 6 || index == 8 || index == 10;
        if (dashBefore) {
            text[position++] = '-';
        }
        text[position++] = digits[bytes[index] >> 4];
        text[position++] = digits[bytes[index] & 0x0F];
    }
    text[position] = '\0';

    JSContext *ctx = js::context();
    return js::Value(ctx, JS_NewString(ctx, text));
}

// ### timers ##################################################################

/**
 * Delay of setTimeout / setInterval in whole milliseconds (fraction dropped).
 * Like Node, NaN, a negative delay or one above 2^31-1 ms (about 24.8 days)
 * becomes 1 ms.
 */
int64_t timerDelay(js::Optional<double> delay) {
    // the comparisons are false for NaN; only a value in range is converted:
    // converting NaN, an infinity or a huge double to an integer is undefined
    const double MAX_DELAY = 2147483647.0;
    double requested = delay.valueOr(0);
    bool inRange = requested >= 0 && requested <= MAX_DELAY;
    if (!inRange) {
        return 1;
    }

    return (int64_t)requested;
}

/**
 * setTimeout(callback, delay = 0, ...arguments): calls callback(...arguments)
 * once after 'delay' ms (see timerDelay); returns the timer id.
 */
int setTimeout(js::Function callback, js::Optional<double> delay, js::Args arguments) {
    return rt::Loop::current()->addTimer(timerDelay(delay), false, callback, arguments);
}

/**
 * setInterval(callback, delay = 0, ...arguments): calls
 * callback(...arguments) every 'delay' ms (see timerDelay); returns the timer id.
 */
int setInterval(js::Function callback, js::Optional<double> delay, js::Args arguments) {
    return rt::Loop::current()->addTimer(timerDelay(delay), true, callback, arguments);
}

/**
 * clearTimeout(id): cancels a timer. Accepts anything, like the Web:
 * clearTimeout(undefined) or an unknown id does nothing.
 */
void clearTimeout(js::Value id) {
    // only a number can name a timer
    double number = 0;
    bool isNumber = JS_IsNumber(id.get()) && JS_ToFloat64(id.context(), &number, id.get()) == 0;

    // timer ids are ints from 1 up: anything outside (NaN included) names no
    // timer, and converting it to an int would be undefined
    bool inRange = isNumber && number >= 1 && number <= 2147483647.0;
    if (inRange) {
        rt::Loop::current()->clearTimer((int)number);
    }
}

/**
 * clearInterval(id): same as clearTimeout (timers share one id space).
 */
void clearInterval(js::Value id) {
    clearTimeout(id);
}

// ### process #################################################################

/**
 * process.exit(code = 0): ends the PROGRAM, never the process. The loop stops
 * with 'code' (no timer, callback or promise job runs any more), the JS code
 * running now is unwound at once, and the runtime is then freed normally (the
 * device agent sends EXITED and stays alive; `qjsx run` returns the code).
 */
void processExit(js::Optional<int> code) {
    // what was written so far comes out before the program ends
    fflush(stdout);
    fflush(stderr);

    // the loop stops at its next check, with this code
    rt::Loop::current()->exit(code.valueOr(0));

    // unwind the running JS with an exception that no catch or finally sees
    // (QuickJS's "uncatchable" flag, the one of an interrupt; set after the
    // throw, which clears it); the loop drops it silently after exit()
    JSContext *ctx = js::context();
    JS_ThrowInternalError(ctx, "process.exit");
    JS_SetUncatchableException(ctx, true);
    js::detail::markPendingException();
}

/**
 * process.cwd(): current directory, with '/' separators (also on Windows,
 * unlike Node): the working folder of the run's permissions if they set one
 * (a device: its data folder; a PC program: its root folder), "/" on a
 * device without file system, else the system's. Throws a system error if
 * it cannot be read.
 */
js::Value cwd() {
    JSContext *ctx = js::context();

    // the run's own folder
    const rt::Permissions *permissions = rt::permissions();
    if (permissions != nullptr && !permissions->hasFileSystem()) {
        return js::Value(ctx, JS_NewString(ctx, "/"));
    }

    if (permissions != nullptr && permissions->workingFolder() != nullptr) {
        return js::Value(ctx, JS_NewString(ctx, permissions->workingFolder()));
    }

    // ask the system (malloc'd string)
    char *directory = files::currentDirectory();
    if (directory == nullptr) {
        return js::failSystem();
    }

    // copy it into a JS string
    js::Value result(ctx, JS_NewString(ctx, directory));
    memory::release(directory);
    return result;
}

// The arguments of process.argv (rt::setProcessArguments), one set per
// thread: each thread that creates a runtime sets its own first (the twin
// agent runs the device and its web page on two threads).
thread_local int processArgumentCount = 0;
thread_local char **processArguments = nullptr;

/**
 * New JS object process.versions (plain properties: printed by
 * JSON.stringify): qjsx and the QuickJS engine. A package run adds 'build',
 * the build id of the package (rt::runPackage).
 */
JSValue newVersions(JSContext *ctx) {
    JSValue versions = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, versions, "qjsx", JS_NewString(ctx, QJSX_VERSION));
    JS_SetPropertyStr(ctx, versions, "quickjs", JS_NewString(ctx, CONFIG_VERSION));
    return versions;
}

/**
 * New JS array with the arguments given to rt::setProcessArguments.
 */
JSValue newArgv(JSContext *ctx) {
    JSValue array = JS_NewArray(ctx);

    for (int i = 0; i < processArgumentCount; i++) {
        JS_SetPropertyUint32(ctx, array, i, JS_NewString(ctx, processArguments[i]));
    }

    return array;
}

/**
 * Adds "NAME=value" to the env object as env.NAME = "value", if the run's
 * permissions let the program read NAME.
 */
void addEnvironmentEntry(JSContext *ctx, JSValueConst env, const char *entry) {
    // the name ends at the first '='; entries without a name are skipped
    const char *equal = strchr(entry, '=');
    if (equal == nullptr || equal == entry) {
        return;   // Windows has entries like "=C:=C:\..."
    }

    // copy the name, zero-terminated
    size_t nameLength = equal - entry;
    char *name = (char *)memory::allocate(nameLength + 1);
    memcpy(name, entry, nameLength);
    name[nameLength] = 0;

    // the value is the rest of the entry; a variable not granted is left out
    const rt::Permissions *permissions = rt::permissions();
    bool allowed = permissions == nullptr || permissions->mayUseEnv(name);
    if (allowed) {
        JS_SetPropertyStr(ctx, env, name, JS_NewString(ctx, equal + 1));
    }

    memory::release(name);
}

/**
 * New JS object with the environment variables (process.env), read once:
 * later changes to the object do not reach the real environment.
 */
JSValue newEnv(JSContext *ctx) {
    JSValue env = JS_NewObject(ctx);
#if defined(_WIN32)
    // Windows: read the wide environment block, a list of zero-terminated
    // entries ending with an empty one, and add each entry in UTF-8
    wchar_t *block = GetEnvironmentStringsW();

    for (wchar_t *entry = block; *entry != 0; entry += wcslen(entry) + 1) {
        int count = WideCharToMultiByte(CP_UTF8, 0, entry, -1, nullptr, 0, nullptr, nullptr);
        char *text = (char *)memory::allocate(count > 0 ? count : 1);
        WideCharToMultiByte(CP_UTF8, 0, entry, -1, text, count, nullptr, nullptr);
        addEnvironmentEntry(ctx, env, text);
        memory::release(text);
    }

    FreeEnvironmentStringsW(block);
#else

    // elsewhere 'environ' is already in bytes
    for (char **entry = environ; *entry != nullptr; entry++) {
        addEnvironmentEntry(ctx, env, *entry);
    }

#endif
    return env;
}

// #############################################################################
// TextEncoder / TextDecoder (UTF-8 only, like most engines guarantee)

/**
 * True if the TextDecoder label names UTF-8, compared without case. Unlike
 * the Web, surrounding spaces are not accepted.
 */
bool isUtf8Label(const js::Str *label) {
    // the names accepted for UTF-8
    const char *accepted[] = { "utf-8", "utf8", "unicode-1-1-utf-8" };

    // compare the label with each name: same length, then same characters
    // once ASCII letters are lowered
    for (const char *name : accepted) {
        bool same = strlen(name) == label->size();

        for (size_t i = 0; same && i < label->size(); i++) {
            char c = label->data()[i];
            char lower = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            same = lower == name[i];
        }

        if (same) {
            return true;
        }
    }

    return false;
}

class TextEncoder {
    JS_CLASS(TextEncoder);

public:
    JS_EXPORT(encoding) const char *const encoding = "utf-8";   // the only encoding supported

    /**
     * encoder.encode(text = ""): the UTF-8 bytes of the text in a new
     * Uint8Array.
     */
    JS_EXPORT(encode) js::Buffer encode(js::Optional<js::Str> text) {
        // no text: an empty array
        if (!text.has()) {
            return js::Buffer::create(0);
        }

        // js::Str already holds the UTF-8 bytes: copy them
        return js::Buffer::copy(text.value().data(), text.value().size());
    }
};

class TextDecoder {
    JS_CLASS(TextDecoder);

public:
    /**
     * new TextDecoder(label = "utf-8"): throws a RangeError for any label
     * other than UTF-8. The options argument (fatal, ignoreBOM) is ignored.
     */
    void init(js::Optional<js::Str> label) {
        if (label.has() && !isUtf8Label(&label.value())) {
            return (void)js::fail<js::RangeError>("TextDecoder: only utf-8 is supported");
        }
    }

    JS_EXPORT(encoding) const char *const encoding = "utf-8";   // the only encoding supported

    /**
     * decoder.decode(bytes): the string whose UTF-8 encoding is 'bytes'
     * (Uint8Array, typed array or ArrayBuffer); "" without argument.
     */
    JS_EXPORT(decode) js::Str decode(js::Optional<js::Bytes> bytes) {
        // no bytes: an empty string
        if (!bytes.has()) {
            return js::Str();
        }

        // the JS string is made from the bytes taken as UTF-8
        return js::Str((const char *)(bytes.value().data()), bytes.value().size());
    }
};

#ifdef QJSX_MEMORY_MAP
/**
 * Measure (temporary, QJSX_MEMORY_MAP): prints the memory map of the board
 * (its heaps, tasks and JS region), then of the JS heap (the allocator's
 * arenas, what QuickJS counts by kind of object), between two marker lines.
 */
void memoryMap(js::Str title) {
    JSRuntime *runtime = JS_GetRuntime(js::context());

    // the board, then the allocator of the engine
    printf("MAP begin %.*s\n", (int)title.size(), title.data());
    board::printMemoryMap();
    JS_DumpMallocMap(runtime);

    // what QuickJS counts
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(runtime, &usage);
    JS_DumpMemoryUsage(stdout, &usage, runtime);
    printf("MAP end\n");
    fflush(stdout);
}

/**
 * Measure (temporary, QJSX_MEMORY_MAP): one full pass of the garbage
 * collector, as the engine runs it when its threshold is reached; the caller
 * times it (idle GC study, 2026-11-16).
 */
void collectGarbage() {
    JS_RunGC(JS_GetRuntime(js::context()));
}
#endif

#ifdef QJSX_ALLOC_HISTOGRAM
/**
 * Measure (temporary, QJSX_ALLOC_HISTOGRAM): the blocks of the JS heap by
 * size, one line "size live made" per size in use, then the total.
 */
js::Str allocHistogram() {
    // room for every line (static: js::Str keeps the pointer, it does not
    // copy the text)
    const JSAllocHistogram *histogram = JS_GetAllocHistogram();
    const size_t capacity = 64 * JS_ALLOC_HISTOGRAM_STEPS + 128;
    static char text[capacity];
    size_t length = 0;

    // one line per size, the live bytes summed
    uint32_t liveBytes = 0;
    for (int step = 0; step < JS_ALLOC_HISTOGRAM_STEPS; step++) {
        if (histogram->made[step] == 0) {
            continue;
        }
        bool larger = step == JS_ALLOC_HISTOGRAM_STEPS - 1;
        liveBytes += larger ? histogram->large_live_bytes : histogram->live[step] * step * 8;
        length += snprintf(text + length, capacity - length, "%s%d %lu %lu\n", larger ? ">=" : "", step * 8,
            (unsigned long)histogram->live[step], (unsigned long)histogram->made[step]);
    }
    length += snprintf(text + length, capacity - length, "live bytes %lu", (unsigned long)liveBytes);

    // the JS string is made from the text when the function returns
    return js::Str(text, length);
}
#endif

} // namespace

/**
 * The global objects installed by rt::installGlobals: console, timers,
 * TextEncoder / TextDecoder, crypto and process.
 */
JS_GLOBALS(runtime, globals) {
    // console: log / info / debug on stdout, warn / error on stderr
    globals.object("console", [](js::ModuleBuilder &console) {
        JS_FUNCTION_AS(console, consoleLog, "log");
        JS_FUNCTION_AS(console, consoleLog, "info");
        JS_FUNCTION_AS(console, consoleLog, "debug");
        JS_FUNCTION_AS(console, consoleWarn, "warn");
        JS_FUNCTION_AS(console, consoleWarn, "error");
    });

    // timers
    JS_FUNCTION(globals, setTimeout);
    JS_FUNCTION(globals, setInterval);
    JS_FUNCTION(globals, clearTimeout);
    JS_FUNCTION(globals, clearInterval);

#ifdef QJSX_MEMORY_MAP
    // measure (temporary): the memory map of the board and of the JS heap
    JS_FUNCTION_AS(globals, memoryMap, "__memoryMap");
    JS_FUNCTION_AS(globals, collectGarbage, "__gc");
#endif

#ifdef QJSX_ALLOC_HISTOGRAM
    // measure (temporary): sizes of the blocks of the JS heap
    JS_FUNCTION_AS(globals, allocHistogram, "__allocHistogram");
#endif

    // UTF-8 encoder and decoder
    JS_ADD_CLASS(globals, TextEncoder);
    JS_ADD_CLASS(globals, TextDecoder);

    // crypto: random values (web standard)
    globals.object("crypto", [](js::ModuleBuilder &crypto) {
        JS_FUNCTION(crypto, getRandomValues);
        JS_FUNCTION(crypto, randomUUID);
    });

    // process: platform, versions, argv and env (read once, now), exit,
    // cwd, stdout.write, stderr.write
    globals.object("process", [](js::ModuleBuilder &process) {
        JSContext *ctx = process.context();
        process.constant("platform", board::platformName());
        process.constant("versions", js::Value(ctx, newVersions(ctx)));
        process.constant("argv", js::Value(ctx, newArgv(ctx)));
        process.constant("env", js::Value(ctx, newEnv(ctx)));
        JS_FUNCTION_AS(process, processExit, "exit");
        JS_FUNCTION(process, cwd);
        process.object("stdout", [](js::ModuleBuilder &stdoutObject) {
            JS_FUNCTION_AS(stdoutObject, writeStdout, "write");
        });
        process.object("stderr", [](js::ModuleBuilder &stderrObject) {
            JS_FUNCTION_AS(stderrObject, writeStderr, "write");
        });
    });
}

namespace rt {

/**
 * Installs the global objects (JS_GLOBALS above) on the context.
 */
bool installGlobals(JSContext *ctx) {
    // jsbind's error messages (uncaught exceptions...) go to the program's
    // stderr output, so through the sink when one is set
    js::setErrorWriter(&writeErrorOutput);
    return jsInstallGlobals_runtime(ctx);
}

/**
 * Stores the sink used by writeOutput (not thread-safe: set it before the
 * program runs).
 */
void setOutputSink(OutputSink *sink, void *opaque) {
    outputSink = sink;
    outputSinkOpaque = opaque;
}

/**
 * Writes the text to the sink if one is set, otherwise to stdout or stderr.
 */
void writeOutput(int stream, const char *text, size_t length) {
    // a sink (the device agent) gets everything
    if (outputSink != nullptr) {
        outputSink(outputSinkOpaque, stream, text, length);
        return;
    }

    // otherwise the standard stream, flushed at once so that stdout and
    // stderr stay in order
    FILE *out = stream == OUTPUT_STDERR ? stderr : stdout;
    fwrite(text, 1, length, out);
    fflush(out);
}

/**
 * Keeps the arguments (not copied) for process.argv.
 */
void setProcessArguments(int argc, char **argv) {
    processArgumentCount = argc;
    processArguments = argv;
}

/**
 * The path as fs opens it: ENOSYS on a device without file system; a
 * relative path joined to the run's working folder if it has one; else as
 * it is (the system resolves a relative path).
 */
char *resolveProgramPath(const char *path) {
    // a device without file system
    size_t pathLength = strlen(path);
    const rt::Permissions *permissions = rt::permissions();
    if (permissions != nullptr && !permissions->hasFileSystem()) {
        errno = ENOSYS;
        return nullptr;
    }

    // absolute, or no working folder: as it is
    const char *folder = permissions != nullptr ? permissions->workingFolder() : nullptr;
    if (folder == nullptr || files::isAbsolute(path)) {
        return memory::copyText(path, pathLength);
    }

    // a relative path: "<working folder>/<path>"
    size_t folderLength = strlen(folder);
    char *joined = (char *)memory::allocate(folderLength + 1 + pathLength + 1);
    memcpy(joined, folder, folderLength);
    joined[folderLength] = '/';
    memcpy(joined + folderLength + 1, path, pathLength + 1);
    return joined;
}

} // namespace rt
