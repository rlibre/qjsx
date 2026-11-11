/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// jsbind - error model: "the first error wins".
//
// Every call coming from JS into C++ opens a CallScope. Reporting an error
// throws the JS exception in the context right away and marks the scope as
// failed; later errors in the same call are ignored, and JS operations done
// by the library are skipped. When the call returns to JS, the binding sees
// the failed scope and returns JS_EXCEPTION.
//
//   int Uart::readChar() {
//       if (!isOpen) return js::fail("port not open");
//       ...
//   }
//
// js::fail() returns a value convertible to any type, so 'return js::fail()'
// works in functions returning a value. C++ forbids returning a value from a
// void function: there, write 'return (void)js::fail(...);' or
// 'js::fail(...); return;'.
#pragma once

#include "jsbind_base.h"

namespace js {

// Kinds of JS errors, used as template arguments: js::fail<js::RangeError>(...)
struct Error {};
struct TypeError {};
struct RangeError {};
struct SyntaxError {};
struct ReferenceError {};
struct InternalError {};

namespace detail {

/**
 * State of one call from JS into C++. The trampolines (jsbind_class.h) put
 * one on their stack; the scopes of nested calls (JS -> C++ -> JS -> C++)
 * form a chain through 'previous'.
 */
struct CallScope {
    JSContext *ctx;       // context of the call
    bool       failed;    // an error was recorded (first error wins)
    CallScope *previous;  // enclosing call; nullptr: outermost

    /**
     * Opens the scope: it becomes the current call until it is destroyed.
     */
    explicit CallScope(JSContext *context);

    /**
     * Closes the scope: the enclosing call becomes current again.
     */
    ~CallScope();
    CallScope(const CallScope &) = delete;
    CallScope &operator=(const CallScope &) = delete;
};

// Innermost active call (one JS thread per runtime, hence thread_local).
inline thread_local CallScope *currentCall = nullptr;

// > 0 while a finalizer runs: no JS may be executed then.
inline thread_local int finalizing = 0;

/**
 * Pushes the scope on the chain of active calls.
 */
inline CallScope::CallScope(JSContext *context)
    : ctx(context), failed(false), previous(currentCall) {
    currentCall = this;
}

/**
 * Pops the scope from the chain of active calls.
 */
inline CallScope::~CallScope() {
    currentCall = previous;
}

/**
 * Throws a plain Error with an already formatted message. If the Error
 * constructor fails, its own exception is the one left pending.
 */
inline void throwError(JSContext *ctx, Error, const char *message) {
    // QuickJS has no JS_Throw function for a plain Error: look up the global
    // Error constructor
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue constructor = JS_GetPropertyStr(ctx, global, "Error");
    JSValue text = JS_NewString(ctx, message);

    // call it (this also records the stack) and throw the new error; if the
    // constructor failed, its exception is already pending
    JSValue error = JS_CallConstructor(ctx, constructor, 1, &text);
    JS_FreeValue(ctx, text);
    JS_FreeValue(ctx, constructor);
    JS_FreeValue(ctx, global);
    if (!JS_IsException(error)) {
        JS_Throw(ctx, error);
    }
}

/**
 * Throws a TypeError with an already formatted message.
 */
inline void throwError(JSContext *ctx, TypeError, const char *message) {
    JS_ThrowTypeError(ctx, "%s", message);
}

/**
 * Throws a RangeError with an already formatted message.
 */
inline void throwError(JSContext *ctx, RangeError, const char *message) {
    JS_ThrowRangeError(ctx, "%s", message);
}

/**
 * Throws a SyntaxError with an already formatted message.
 */
inline void throwError(JSContext *ctx, SyntaxError, const char *message) {
    JS_ThrowSyntaxError(ctx, "%s", message);
}

/**
 * Throws a ReferenceError with an already formatted message.
 */
inline void throwError(JSContext *ctx, ReferenceError, const char *message) {
    JS_ThrowReferenceError(ctx, "%s", message);
}

/**
 * Throws an InternalError with an already formatted message.
 */
inline void throwError(JSContext *ctx, InternalError, const char *message) {
    JS_ThrowInternalError(ctx, "%s", message);
}

/**
 * Records an error in the current call: throws it and marks the call as failed.
 * False if not recorded: an earlier error won, or no call (printed on stderr).
 */
template<class Kind>
inline bool recordError(const char *message) {
    // no JS call to fail: the message can only be printed
    CallScope *scope = currentCall;
    if (scope == nullptr) {
        fprintf(stderr, "jsbind: error outside of a JS call: %s\n", message);
        return false;
    }

    // the first error wins: later ones are ignored
    if (scope->failed) {
        return false;
    }

    // throw it in the call's context and mark the call
    throwError(scope->ctx, Kind(), message);
    scope->failed = true;
    return true;
}

/**
 * The JS exception is already pending in the context (it comes from JS code
 * called by C++, or from a QuickJS API that failed): only mark the call.
 */
inline void markPendingException() {
    if (currentCall != nullptr) {
        currentCall->failed = true;
    }
}

/**
 * Node-like name of an errno value ("ENOENT"), "EUNKNOWN" for the others.
 */
inline const char *errnoName(int error) {
    switch (error) {
        case EPERM: {
            return "EPERM";
        }
        case ENOENT: {
            return "ENOENT";
        }
        case EIO: {
            return "EIO";
        }
        case EBADF: {
            return "EBADF";
        }
        case EAGAIN: {
            return "EAGAIN";
        }
        case ENOMEM: {
            return "ENOMEM";
        }
        case EACCES: {
            return "EACCES";
        }
        case EBUSY: {
            return "EBUSY";
        }
        case EEXIST: {
            return "EEXIST";
        }
        case ENOTDIR: {
            return "ENOTDIR";
        }
        case EISDIR: {
            return "EISDIR";
        }
        case EINVAL: {
            return "EINVAL";
        }
        case EMFILE: {
            return "EMFILE";
        }
        case ENOSPC: {
            return "ENOSPC";
        }
        case EROFS: {
            return "EROFS";
        }
        case EPIPE: {
            return "EPIPE";
        }
        case ENOTEMPTY: {
            return "ENOTEMPTY";
        }

    #ifdef ETIMEDOUT
        case ETIMEDOUT: {
            return "ETIMEDOUT";
        }

    #endif
    #ifdef ECONNREFUSED
        case ECONNREFUSED: {
            return "ECONNREFUSED";
        }

    #endif
    #ifdef ECONNRESET
        case ECONNRESET: {
            return "ECONNRESET";
        }

    #endif
    #ifdef EADDRINUSE
        case EADDRINUSE: {
            return "EADDRINUSE";
        }

    #endif
        default: {
            return "EUNKNOWN";
        }
    }
}

} // namespace detail

/**
 * Returned by js::fail(): converts to the default value of any type. The value
 * is never seen by JS, the exception wins.
 */
struct Failure {
    /**
     * Conversion to any return type: a default-constructed T.
     */
    template<class T>
    operator T() const {
        return T();
    }
};

/**
 * True if the current call has already failed.
 */
inline bool failed() {
    return detail::currentCall != nullptr && detail::currentCall->failed;
}

/**
 * Context of the current call from JS (nullptr outside of any call).
 */
inline JSContext *context() {
    return detail::currentCall != nullptr ? detail::currentCall->ctx : nullptr;
}

/**
 * Reports an error of the given kind (default Error), printf-like message
 * truncated to 511 bytes. 'return js::fail(...)' works in any function.
 */
template<class Kind = Error>
inline Failure __attribute__((format(printf, 1, 2))) fail(const char *format, ...) {
    // format the message
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    // record it: only the first error of the call is thrown (see recordError)
    detail::recordError<Kind>(message);
    return Failure();
}

/**
 * Reports an error built from errno (or 'error'), with Node-like properties
 * 'code' ("ENOENT") and 'errno'; 'what' (a path...) ends the message.
 */
inline Failure failSystem(int error = errno, const char *what = nullptr) {
    // message "ENOENT: <strerror>[, what]"
    char message[512];
    const char *name = detail::errnoName(error);
    if (what != nullptr) {
        snprintf(message, sizeof(message), "%s: %s, %s", name, strerror(error), what);
    } else {
        snprintf(message, sizeof(message), "%s: %s", name, strerror(error));
    }

    // record it; if it won, take the exception just thrown back out of the
    // context, add 'code' and 'errno', and throw it again
    bool recorded = detail::recordError<Error>(message);
    if (recorded) {
        JSContext *ctx = detail::currentCall->ctx;
        JSValue exception = JS_GetException(ctx);
        JS_SetPropertyStr(ctx, exception, "code", JS_NewString(ctx, name));
        JS_SetPropertyStr(ctx, exception, "errno", JS_NewInt32(ctx, error));
        JS_Throw(ctx, exception);
    }

    return Failure();
}

/**
 * Creates a JS error object without throwing it (used to reject promises).
 * The caller owns the returned value.
 */
template<class Kind = Error>
inline JSValue makeError(JSContext *ctx, const char *message) {
    // the only way to build it: throw it, then take it back out of the context
    detail::throwError(ctx, Kind(), message);
    return JS_GetException(ctx);
}

// Where reportException() writes (default: stderr). The runtime redirects it,
// e.g. to the host when a device agent session is open.
typedef void ErrorWriter(const char *text, size_t length);

namespace detail {
inline ErrorWriter *errorWriter = nullptr;

/**
 * Writes a text through the installed ErrorWriter, or on stderr if none.
 */
inline void writeError(const char *text) {
    if (errorWriter != nullptr) {
        errorWriter(text, strlen(text));
    } else {
        fputs(text, stderr);
    }
}
} // namespace detail

/**
 * Installs the writer used by reportException() (nullptr: back to stderr).
 */
inline void setErrorWriter(ErrorWriter *writer) {
    detail::errorWriter = writer;
}

/**
 * Prints the exception pending in ctx with its stack (through the ErrorWriter),
 * and clears it.
 */
inline void reportException(JSContext *ctx, const char *prefix = "Uncaught") {
    // take the exception (this clears it) and print "<prefix> <message>",
    // truncated to 511 bytes
    JSValue exception = JS_GetException(ctx);
    const char *text = JS_ToCString(ctx, exception);
    char line[512];
    snprintf(line, sizeof(line), "%s %s\n", prefix, text != nullptr ? text : "(exception)");
    detail::writeError(line);
    JS_FreeCString(ctx, text);

    // then its stack, which only Error objects have
    if (JS_IsError(ctx, exception)) {
        JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
        if (!JS_IsUndefined(stack)) {
            const char *stackText = JS_ToCString(ctx, stack);
            if (stackText != nullptr) {
                detail::writeError(stackText);
            }

            JS_FreeCString(ctx, stackText);
        }

        JS_FreeValue(ctx, stack);
    }

    JS_FreeValue(ctx, exception);
}

namespace detail {

/**
 * Hands on the exception pending in ctx: inside a call from JS, the call is
 * marked failed and the exception reaches JS when it returns; outside of any
 * call, it is printed as uncaught and cleared.
 */
inline void propagateException(JSContext *ctx) {
    if (currentCall != nullptr) {
        markPendingException();
    } else {
        reportException(ctx);
    }
}

} // namespace detail
} // namespace js
