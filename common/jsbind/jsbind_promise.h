// jsbind - promises returned by C++ methods.
//
//   js::Promise<js::Buffer> Uart::read(int size)
//   {
//       auto promise = js::Promise<js::Buffer>::create();
//       pendingRead = promise.resolver();    // member: kept and marked for the GC
//       return promise;
//   }
//   ... later, when the data arrives (on the JS thread):
//       pendingRead.resolve(buffer);   or   pendingRead.reject("timeout");
#pragma once

#include "jsbind_convert.h"

namespace js {

/**
 * The resolve and reject functions of a pending promise. Settling it calls
 * one of them and clears both, so the promise is settled at most once
 * through this resolver.
 */
template<class T>
class Resolver {
public:
    Resolver() = default;

    /**
     * Takes the two functions given by JS_NewPromiseCapability.
     */
    Resolver(Function resolve, Function reject) : _resolve(meta::move(resolve)), _reject(meta::move(reject)) {}

    /**
     * True until resolve() or reject() is called.
     */
    bool     pending() const {
        return !_resolve.isUndefined();
    }

    /**
     * Fulfills the promise with 'value' (converted with Convert<T>::to); the
     * resolver is no longer pending. Does nothing if it was not pending.
     */
    void resolve(const T &value) {
        // already settled, or empty: nothing to settle
        if (!pending()) {
            return;
        }

        // clear the resolver first (settled at most once); a copy of the
        // function keeps it alive meanwhile
        Function resolveFunction = _resolve;
        clear();

        // then call it
        resolveFunction(value);
    }

    /**
     * Rejects the promise with a new error of the given kind (default:
     * Error) and a printf-like message (truncated to 511 bytes). Does nothing
     * if the resolver was not pending.
     */
    template<class Kind = Error>
    void __attribute__((format(printf, 2, 3))) reject(const char *format, ...) {
        // already settled, or empty: no context to build the error in, and
        // nothing to settle
        if (!pending()) {
            return;
        }

        // format the message
        char message[512];
        va_list args;
        va_start(args, format);
        vsnprintf(message, sizeof(message), format, args);
        va_end(args);

        // reject with a new error of that kind
        rejectWithError(makeError<Kind>(_reject.context(), message));
    }

    /**
     * Rejects the promise with a system error built from an errno value,
     * with the properties 'code' ("ENOENT") and 'errno' (like failSystem).
     * Does nothing if the resolver was not pending.
     */
    void rejectSystem(int error) {
        // already settled, or empty: no context to build the error in, and
        // nothing to settle
        if (!pending()) {
            return;
        }

        // message "ENOENT: <strerror>"
        JSContext *ctx = _reject.context();
        char message[256];
        const char *name = detail::errnoName(error);
        snprintf(message, sizeof(message), "%s: %s", name, strerror(error));

        // the error, with Node-like 'code' and 'errno', is the reason
        JSValue exception = makeError<Error>(ctx, message);
        JS_SetPropertyStr(ctx, exception, "code", JS_NewString(ctx, name));
        JS_SetPropertyStr(ctx, exception, "errno", JS_NewInt32(ctx, error));
        rejectWithError(exception);
    }

protected:
    /**
     * Rejects the promise with 'error' (taken: owned by this function).
     */
    void rejectWithError(JSValue error) {
        // clear the resolver first (settled at most once); copies of the
        // function and of the reason keep them alive meanwhile
        Function rejectFunction = _reject;
        Value reason(rejectFunction.context(), error);
        clear();

        // then call it
        rejectFunction(reason);
    }

    /**
     * Drops both functions: the resolver is no longer pending.
     */
    void clear() {
        _resolve.reset();
        _reject.reset();
    }

    Function _resolve;  // resolve function of the promise; empty once settled
    Function _reject;   // reject function; empty once settled
};

/**
 * Promise without value: resolve() takes no argument.
 */
template<>
class Resolver<void> : public Resolver<Value> {
public:
    Resolver() = default;

    /**
     * Takes the two functions given by JS_NewPromiseCapability.
     */
    Resolver(Function resolve, Function reject) : Resolver<Value>(meta::move(resolve), meta::move(reject)) {}

    /**
     * Fulfills the promise with undefined.
     */
    void resolve() {
        Resolver<Value>::resolve(Value());
    }
};

/**
 * A JS promise returned by a C++ function, with the resolver that settles it.
 */
template<class T>
class Promise {
public:
    Promise() = default;

    /**
     * New pending promise, in 'ctx' or the current call's context. On failure:
     * an empty promise (resolver not pending), the error recorded in the
     * current call, or printed outside of any call (also when there is no
     * context at all). Nothing is created once the current call has failed.
     */
    static Promise create(JSContext *ctx = nullptr) {
        // the given context, else the current call's; none: nowhere to create it
        JSContext *target = ctx != nullptr ? ctx : js::context();
        Promise result;
        if (target == nullptr) {
            fail("Promise.create: no context (outside of a JS call, pass one)");
            return result;
        }

        // the first error wins: once the current call failed, nothing is created
        if (failed()) {
            return result;
        }

        // create the promise and its resolve / reject functions
        JSValue functions[2];
        JSValue promise = JS_NewPromiseCapability(target, functions);
        if (JS_IsException(promise)) {
            detail::propagateException(target);
            return result;
        }

        // keep the promise; the resolver takes the two functions
        result._promise = Value(target, promise);
        result._resolver = Resolver<T>(Function(target, functions[0]), Function(target, functions[1]));
        return result;
    }

    /**
     * The resolver, to copy into a member until the result is known.
     */
    const Resolver<T> &resolver() const {
        return _resolver;
    }

    /**
     * The JS promise object.
     */
    const Value       &value() const {
        return _promise;
    }

private:
    Value       _promise;   // the JS promise; undefined if creation failed
    Resolver<T> _resolver;  // settles _promise
};

template<class T>
struct Convert<Promise<T>> {
    /**
     * New reference on the JS promise (returned to JS by a bound function).
     */
    static JSValue to(JSContext *, const Promise<T> &promise) {
        return promise.value().dup();
    }
};

} // namespace js
