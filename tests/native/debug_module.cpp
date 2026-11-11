// Native module "debug_test": exposes the debugger API of the engine
// (quickjs-debug.h) to tests/debug.test.js. Only built into espjs-tests.
//
//   setHook(callback | null)   callback(event) is called for each stop, with
//                              { reason, exception, atReturn, returnValue };
//                              while it runs, frame() / slot() describe the
//                              stopped frames. setHook(null) before the end of
//                              the test (the callback is a GC root).
//   functionLocation(fn)       { module, number }
//   setStops(module, number, "breakpoints" | "statements", offsets) -> bool
//   setStepping(bool), setExceptionReporting(bool)
//   frame(level)               { id, isBytecode, module, number, pc,
//                                argumentCount, localCount, closureCount } or null
//   slot(level, "argument" | "local" | "closure", index)
//                              the value, UNINITIALIZED (exported string) for
//                              a variable in its temporal dead zone
//   setSlot(level, kind, index, value) -> bool
#include "jsbind.h"
#include "quickjs-debug.h"
#include "modules.h"

namespace {

JS_ENUM(StopKind, breakpoints, statements);
JS_ENUM(SlotKind, argument, local, closure);

const char *const UNINITIALIZED = "<uninitialized>";

// The callback of setHook(): a raw value, freed by setHook(null) (a js::Value
// at namespace scope would be destroyed after the runtime).
JSValue hookCallback = JS_UNDEFINED;

/**
 * The name of a stop reason, as the tests see it in event.reason.
 */
const char *reasonName(JSDebugReason reason) {
    switch (reason) {
        case JS_DEBUG_REASON_BREAKPOINT: {
            return "breakpoint";
        }
        case JS_DEBUG_REASON_STEP: {
            return "step";
        }
        case JS_DEBUG_REASON_STATEMENT: {
            return "statement";
        }
        case JS_DEBUG_REASON_EXCEPTION: {
            return "exception";
        }
    }

    return "?";
}

/**
 * The event given to the JS callback: { reason, exception, atReturn,
 * returnValue } (the values are duplicated: the event only borrows them).
 */
JSValue eventToJs(JSContext *ctx, const JSDebugEvent *event) {
    JSValue object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, object, "reason", JS_NewString(ctx, reasonName(event->reason)));
    JS_SetPropertyStr(ctx, object, "exception", JS_DupValue(ctx, event->exception));
    JS_SetPropertyStr(ctx, object, "atReturn", JS_NewBool(ctx, event->at_return));
    JS_SetPropertyStr(ctx, object, "returnValue", JS_DupValue(ctx, event->return_value));
    return object;
}

// An exception of the callback is printed and makes the test fail later
// (hookErrors), it does not stop the debugged code.
int hookErrors = 0;

/**
 * Debug hook installed by setHook(), called by the interpreter at each stop (on
 * the JS thread, inside the stopped code): calls the JS callback. Always returns 0: continue.
 */
int debugHook(JSContext *ctx, const JSDebugEvent *event, void *) {
    // call the JS callback with the event
    JSValue argument = eventToJs(ctx, event);
    JSValue result = JS_Call(ctx, hookCallback, JS_UNDEFINED, 1, &argument);
    JS_FreeValue(ctx, argument);

    // the callback threw: count it (hookErrors), print it and clear it, so
    // that the debugged code goes on
    if (JS_IsException(result)) {
        hookErrors++;
        JSValue error = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, error);
        fprintf(stderr, "debug hook callback failed: %s\n", message != nullptr ? message : "?");
        JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, error);
    }

    JS_FreeValue(ctx, result);
    return 0;
}

/**
 * JS setHook(callback | null): installs debugHook with this callback, or removes
 * the hook for null / undefined.
 */
void setHook(js::Value callback) {
    JSContext *ctx = js::context();
    JSRuntime *rt = JS_GetRuntime(ctx);

    // free the previous callback
    JS_FreeValue(ctx, hookCallback);
    hookCallback = JS_UNDEFINED;

    // null / undefined: no hook
    if (callback.isNull() || callback.isUndefined()) {
        JS_SetDebugHook(rt, nullptr, nullptr);
        return;
    }

    // keep the new callback (a GC root until setHook(null)) and install the hook
    hookCallback = callback.dup();
    JS_SetDebugHook(rt, debugHook, nullptr);
}

/**
 * JS hookErrorCount(): number of exceptions thrown by the hook callback.
 */
int hookErrorCount() {
    return hookErrors;
}

/**
 * The string of an atom, or null for JS_ATOM_NULL.
 */
js::Value atomToJs(JSContext *ctx, JSAtom atom) {
    if (atom == JS_ATOM_NULL) {
        return js::Value(ctx, JS_NULL);
    }

    return js::Value(ctx, JS_AtomToString(ctx, atom));
}

/**
 * JS functionLocation(fn): { module, number } of a bytecode function;
 * throws TypeError for any other value.
 */
js::Value functionLocation(js::Value function) {
    // ask the engine (the module atom is borrowed)
    JSContext *ctx = js::context();
    JSAtom module = JS_ATOM_NULL;
    int number = 0;
    if (JS_DebugGetFunctionLocation(ctx, function.get(), &module, &number) < 0) {
        return js::fail<js::TypeError>("not a bytecode function");
    }

    // build { module, number }
    JSValue location = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, location, "module", atomToJs(ctx, module).dup());
    JS_SetPropertyStr(ctx, location, "number", JS_NewInt32(ctx, number));
    return js::Value(ctx, location);
}

/**
 * JS setStops(module, number, kind, offsets): sets the breakpoint or statement
 * stops of a function. Returns true on success.
 */
bool setStops(js::Str module, int number, StopKind kind, js::Value offsetArray) {
    JSContext *ctx = js::context();

    // length of the JS array of bytecode offsets
    JSValue lengthValue = JS_GetPropertyStr(ctx, offsetArray.get(), "length");
    uint32_t length = 0;
    JS_ToUint32(ctx, &length, lengthValue);
    JS_FreeValue(ctx, lengthValue);

    // a C array of length + 1 elements: never a zero-sized allocation
    uint32_t *offsets = (uint32_t *)js_malloc(ctx, sizeof(uint32_t) * (length + 1));
    if (offsets == nullptr) {
        return js::fail("out of memory");
    }

    // copy the offsets into it
    for (uint32_t i = 0; i < length; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, offsetArray.get(), i);
        JS_ToUint32(ctx, &offsets[i], item);
        JS_FreeValue(ctx, item);
    }

    // give it to the engine, then free it
    JSDebugStopKind engineKind = kind == StopKind::breakpoints ? JS_DEBUG_STOPS_BREAKPOINTS : JS_DEBUG_STOPS_STATEMENTS;
    int result = JS_DebugSetStops(ctx, module.data(), number, engineKind, offsets, (int)length);
    js_free(ctx, offsets);
    return result == 0;
}

/**
 * JS setStepping(bool): when on, the hook is called at every statement
 * start set by setStops(..., "statements", ...).
 */
void setStepping(bool stepping) {
    JS_DebugSetStepping(JS_GetRuntime(js::context()), stepping);
}

/**
 * JS setExceptionReporting(bool): when on, the hook is called once for
 * each thrown value, in the frame of the throw.
 */
void setExceptionReporting(bool report) {
    JS_DebugSetExceptionReporting(JS_GetRuntime(js::context()), report);
}

/**
 * JS frame(level): the frame at 'level' of the stopped stack (0 = the
 * innermost) as an object, or null if there is no such frame.
 */
js::Value frame(int level) {
    // ask the engine for the frame
    JSContext *ctx = js::context();
    JSDebugFrame info;
    if (JS_DebugGetFrame(ctx, level, &info) < 0) {
        return js::Value(ctx, JS_NULL);
    }

    // copy its description into an object (the module atom is borrowed)
    JSValue object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, object, "id", JS_NewFloat64(ctx, (double)info.id));
    JS_SetPropertyStr(ctx, object, "isBytecode", JS_NewBool(ctx, info.is_bytecode));
    JS_SetPropertyStr(ctx, object, "module", atomToJs(ctx, info.module_name).dup());
    JS_SetPropertyStr(ctx, object, "number", JS_NewInt32(ctx, info.function_number));
    JS_SetPropertyStr(ctx, object, "pc", JS_NewInt64(ctx, info.pc));
    JS_SetPropertyStr(ctx, object, "argumentCount", JS_NewInt32(ctx, info.argument_count));
    JS_SetPropertyStr(ctx, object, "localCount", JS_NewInt32(ctx, info.local_count));
    JS_SetPropertyStr(ctx, object, "closureCount", JS_NewInt32(ctx, info.closure_count));
    return js::Value(ctx, object);
}

/**
 * The engine's slot kind for a SlotKind.
 */
JSDebugSlotKind engineSlotKind(SlotKind kind) {
    switch (kind) {
        case SlotKind::argument: {
            return JS_DEBUG_SLOT_ARGUMENT;
        }
        case SlotKind::local: {
            return JS_DEBUG_SLOT_LOCAL;
        }
        case SlotKind::closure: {
            return JS_DEBUG_SLOT_CLOSURE;
        }
    }

    return JS_DEBUG_SLOT_LOCAL;
}

/**
 * JS slot(level, kind, index): the value of a variable of a stopped frame.
 * A variable in its temporal dead zone gives the string UNINITIALIZED.
 * Throws RangeError if there is no such frame or slot.
 */
js::Value slot(int level, SlotKind kind, int index) {
    // read the slot
    JSContext *ctx = js::context();
    JSValue value = JS_DebugGetSlot(ctx, level, engineSlotKind(kind), index);
    if (JS_IsException(value)) {
        return js::fail<js::RangeError>("no such frame or slot");
    }

    // an uninitialized value must not reach JS code
    if (JS_VALUE_GET_TAG(value) == JS_TAG_UNINITIALIZED) {
        return js::Value(ctx, JS_NewString(ctx, UNINITIALIZED));
    }

    return js::Value(ctx, value);
}

/**
 * JS setSlot(level, kind, index, value): replaces the value of a variable
 * of a stopped frame (the engine takes the duplicated value). Returns true
 * on success.
 */
bool setSlot(int level, SlotKind kind, int index, js::Value value) {
    JSContext *ctx = js::context();
    return JS_DebugSetSlot(ctx, level, engineSlotKind(kind), index, value.dup()) == 0;
}

} // namespace

/**
 * Module "debug_test": the functions above and the UNINITIALIZED marker.
 */
JS_MODULE(debug_test, module) {
    JS_FUNCTION(module, setHook);
    JS_FUNCTION(module, hookErrorCount);
    JS_FUNCTION(module, functionLocation);
    JS_FUNCTION(module, setStops);
    JS_FUNCTION(module, setStepping);
    JS_FUNCTION(module, setExceptionReporting);
    JS_FUNCTION(module, frame);
    JS_FUNCTION(module, slot);
    JS_FUNCTION(module, setSlot);
    module.constant("UNINITIALIZED", UNINITIALIZED);
}

/**
 * Registers the module "debug_test" (called by registerTestModules()).
 */
void registerDebugTestModule() {
    rt::registerNativeModule("debug_test", &jsNewModule_debug_test);
}
