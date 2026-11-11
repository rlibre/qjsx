/*
 * espjs: debugger support in the QuickJS engine.
 *
 * Not part of upstream QuickJS: see doc/engine-changes.md. Implemented in
 * core/debug.c, with hooks in core/interpreter.c.
 *
 * Model
 * -----
 * The engine knows nothing about source positions on the target (the
 * bytecode is stripped): the debugger (the "stub") gives it, per function,
 * bytecode offsets:
 *   - breakpoints: offsets where the hook is always called;
 *   - statements:  offsets of the statement starts, where the hook is called
 *                  while stepping is on (JS_DebugSetStepping).
 * A function is identified by its module name and its function number (the
 * number given by the compiler, in compilation order, stored in the bytecode
 * and in the debug sidecar).
 *
 * The hook is called on the JS thread, inside the interpreter, before the
 * instruction at the stop offset is executed. While it runs, it may inspect
 * frames and slots and call JS functions (evaluation): no nested hook call
 * happens meanwhile. Returning -1 from the hook throws the pending exception
 * (set by the hook) at the stop position.
 *
 * Functions that are not running bytecode with stops pay nothing: a frame
 * uses the debug dispatch table only when the hook is set and its function
 * has breakpoints, or has statements while stepping is on.
 */
#ifndef QUICKJS_DEBUG_H
#define QUICKJS_DEBUG_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum JSDebugReason {
    JS_DEBUG_REASON_BREAKPOINT,  /* a breakpoint offset was reached */
    JS_DEBUG_REASON_STEP,        /* stepping: a statement start was reached */
    JS_DEBUG_REASON_STATEMENT,   /* a 'debugger;' statement */
    JS_DEBUG_REASON_EXCEPTION,   /* a value was thrown, before unwinding */
} JSDebugReason;

typedef struct JSDebugEvent {
    JSDebugReason reason;
    /* JS_DEBUG_REASON_EXCEPTION: the thrown value (borrowed), otherwise
       JS_UNDEFINED. While the hook runs there is no pending exception: the
       engine puts it back when the hook returns. */
    JSValueConst exception;
    /* TRUE when the frame stops on a return instruction (breakpoint or step
       on a JS_DEBUG_POSITION_RETURN position): 'return_value' (borrowed) is
       the value about to be returned. */
    JS_BOOL at_return;
    JSValueConst return_value;
} JSDebugEvent;

/* Returns 0 to continue, -1 to throw the exception pending in 'ctx'
   (ignored for JS_DEBUG_REASON_EXCEPTION: the exception goes on anyway). */
typedef int JSDebugHook(JSContext *ctx, const JSDebugEvent *event, void *opaque);

/* Installs the hook (NULL removes it). Without hook, nothing is ever called. */
void JS_SetDebugHook(JSRuntime *rt, JSDebugHook *hook, void *opaque);

/* Stepping on: the hook is called at every statement start of the
   functions that have statements (JS_DEBUG_STOPS_STATEMENTS). The hook
   decides by comparing frames whether it is a stop (step over, out). A
   pause request is stepping on. */
void JS_DebugSetStepping(JSRuntime *rt, JS_BOOL stepping);

/* Exceptions reported on: the hook is called once per thrown value, in the
   frame of the throw. A value rethrown by a 'finally' clause is not
   reported again. */
void JS_DebugSetExceptionReporting(JSRuntime *rt, JS_BOOL report);

typedef enum JSDebugStopKind {
    JS_DEBUG_STOPS_BREAKPOINTS,
    JS_DEBUG_STOPS_STATEMENTS,
} JSDebugStopKind;

/* Replaces the stops of one kind of function 'function_number' of the loaded
   module 'module_name' by the bytecode offsets 'offsets[0..count-1]'
   (count = 0 removes them). Returns 0, or -1 if the module or the function
   is not found or an offset is outside the function (nothing changed, no
   exception thrown). The stops live as long as the function's bytecode:
   they must be set again when the module is loaded again. */
int JS_DebugSetStops(JSContext *ctx, const char *module_name, int function_number,
                     JSDebugStopKind kind, const uint32_t *offsets, int count);

/* Removes the stops of every function of the loaded modules (debugger
   detached). */
void JS_DebugClearAllStops(JSContext *ctx);

/* Module name (borrowed atom) and function number of a bytecode function
   object (for CDP [[FunctionLocation]], or to find the stops of a function).
   Returns 0, or -1 if 'function' is not a bytecode function. Usable at any
   time. */
int JS_DebugGetFunctionLocation(JSContext *ctx, JSValueConst function,
                                JSAtom *module_name, int *function_number);

/* Objects, described without running any JS code (no getter, no proxy
   trap): what the debugger shows in previews and the Variables pane. */
typedef enum JSDebugObjectKind {
    JS_DEBUG_OBJECT_PLAIN,       /* any other object */
    JS_DEBUG_OBJECT_FUNCTION,
    JS_DEBUG_OBJECT_ARRAY,       /* Array, arguments */
    JS_DEBUG_OBJECT_ERROR,
    JS_DEBUG_OBJECT_DATE,
    JS_DEBUG_OBJECT_REGEXP,
    JS_DEBUG_OBJECT_MAP,
    JS_DEBUG_OBJECT_SET,
    JS_DEBUG_OBJECT_WEAKMAP,
    JS_DEBUG_OBJECT_WEAKSET,
    JS_DEBUG_OBJECT_PROMISE,
    JS_DEBUG_OBJECT_PROXY,
    JS_DEBUG_OBJECT_TYPED_ARRAY,
    JS_DEBUG_OBJECT_ARRAY_BUFFER,
    JS_DEBUG_OBJECT_DATAVIEW,
    JS_DEBUG_OBJECT_GENERATOR,
    JS_DEBUG_OBJECT_ITERATOR,
    JS_DEBUG_OBJECT_WRAPPER,     /* Number, String, Boolean, Symbol, BigInt objects */
    JS_DEBUG_OBJECT_WEAKREF,
} JSDebugObjectKind;

typedef struct JSDebugObjectInfo {
    JSDebugObjectKind kind;
    JSAtom class_name;   /* name of the object's class ("Object", "Map"...), borrowed */
    /* Map / Set size, typed array length, ArrayBuffer byte length, else -1
       (the length of an Array is its "length" property) */
    int64_t size;
} JSDebugObjectInfo;

/* Returns 0, or -1 if 'obj' is not an object. */
int JS_DebugGetObjectInfo(JSContext *ctx, JSValueConst obj, JSDebugObjectInfo *info);

typedef enum JSDebugInternal {
    JS_DEBUG_INTERNAL_PROXY_TARGET,
    JS_DEBUG_INTERNAL_PROXY_HANDLER,
    JS_DEBUG_INTERNAL_PRIMITIVE_VALUE,   /* of a wrapper object or a Date (time value) */
    JS_DEBUG_INTERNAL_BOUND_TARGET,      /* target function of a bound function */
} JSDebugInternal;

/* An internal slot of an object (new reference), JS_UNDEFINED if the object
   has no such slot. */
JSValue JS_DebugGetInternal(JSContext *ctx, JSValueConst obj, JSDebugInternal which);

/* Frames, from the frame where the hook was called (level 0) to the
   outermost one. Only valid while the hook runs. */
typedef struct JSDebugFrame {
    /* identity of the frame, stable while the frame exists: compares frames
       between two hook calls (stepping). Address of the frame record (on
       the C stack, or in the state of a generator / async function). */
    uintptr_t id;
    JS_BOOL is_bytecode;     /* FALSE: native function, the fields below are 0 */
    JSAtom module_name;      /* borrowed, JS_ATOM_NULL if unknown */
    int function_number;
    /* offset in the bytecode: the instruction about to run at level 0 for a
       breakpoint, step or 'debugger;'; somewhere inside the throwing
       instruction for an exception; just after the call instruction for
       the calling frames (levels > 0). */
    uint32_t pc;
    int argument_count;      /* slots of kind JS_DEBUG_SLOT_ARGUMENT */
    int local_count;         /* slots of kind JS_DEBUG_SLOT_LOCAL */
    int closure_count;       /* slots of kind JS_DEBUG_SLOT_CLOSURE */
} JSDebugFrame;

/* Returns 0 and fills 'frame', or -1 if there is no frame at 'level' (or the
   hook is not running). */
int JS_DebugGetFrame(JSContext *ctx, int level, JSDebugFrame *frame);

/* The function object of the frame at 'level' (new reference), or
   JS_UNDEFINED. */
JSValue JS_DebugGetFrameFunction(JSContext *ctx, int level);

typedef enum JSDebugSlotKind {
    JS_DEBUG_SLOT_ARGUMENT,  /* argument, by index */
    JS_DEBUG_SLOT_LOCAL,     /* local variable, by index (var_buf) */
    JS_DEBUG_SLOT_CLOSURE,   /* variable captured from an enclosing function */
} JSDebugSlotKind;

/* Value of a slot of the bytecode frame at 'level' (new reference), or
   JS_EXCEPTION if there is no such frame or slot (no exception thrown).
   A lexical variable before its declaration gives a value with the tag
   JS_TAG_UNINITIALIZED (temporal dead zone). */
JSValue JS_DebugGetSlot(JSContext *ctx, int level, JSDebugSlotKind kind, int index);

/* Replaces the value of a slot (takes ownership of 'value'). Returns 0, or
   -1 if there is no such frame or slot (value freed, no exception thrown). */
int JS_DebugSetSlot(JSContext *ctx, int level, JSDebugSlotKind kind, int index, JSValue value);

/* ---------------------------------------------------------------------- */
/* Debug info of the compiled functions ("sidecar"), for the host compiler.

   When a collector is set, the compiler calls it once per compiled
   function, children before their parent (the top-level function comes
   last). The bytecode does not depend on it: the same code is produced
   with or without collector. Source positions are byte offsets in the
   compiled source (UTF-8). Everything given to the collector is borrowed
   and only valid during the call. */

typedef enum JSDebugPositionKind {
    /* position of an instruction that can throw or call (as in pc2line) */
    JS_DEBUG_POSITION_EXPRESSION,
    /* start of a statement, of a 'catch' clause or of a loop test or
       update: where stepping and breakpoints stop */
    JS_DEBUG_POSITION_STATEMENT,
    /* a return instruction (explicit 'return' or end of the function):
       a stop there shows the returned value */
    JS_DEBUG_POSITION_RETURN,
} JSDebugPositionKind;

typedef struct JSDebugPosition {
    uint32_t pc;          /* bytecode offset of the instruction */
    uint32_t source_pos;  /* source offset */
    JSDebugPositionKind kind;
} JSDebugPosition;

typedef struct JSDebugScope {
    int parent;               /* index of the enclosing scope, -1 for scope 0 */
    uint32_t source_start;    /* source range of the scope: [start, end) */
    uint32_t source_end;
} JSDebugScope;

#define JS_DEBUG_VARIABLE_LEXICAL   (1 << 0)  /* let, const, class */
#define JS_DEBUG_VARIABLE_CONST     (1 << 1)
#define JS_DEBUG_VARIABLE_CAPTURED  (1 << 2)  /* used by an inner function */
#define JS_DEBUG_VARIABLE_GLOBAL    (1 << 3)  /* closure: a global variable */

typedef struct JSDebugVariable {
    JSAtom name;   /* internal variables have names like "<ret>" or "this" */
    int scope;     /* index in 'scopes' (local variables only), else -1 */
    int flags;     /* JS_DEBUG_VARIABLE_* */
} JSDebugVariable;

typedef struct JSDebugFunctionInfo {
    int function_number;
    JSAtom name;                /* JS_ATOM_NULL if anonymous */
    uint32_t source_start;      /* source range of the function */
    uint32_t source_end;
    uint32_t bytecode_length;
    /* sorted by pc; several positions may share a pc */
    const JSDebugPosition *positions;
    int position_count;
    /* slot index = index in these arrays (JSDebugSlotKind) */
    const JSDebugVariable *arguments;
    int argument_count;
    const JSDebugVariable *locals;
    int local_count;
    const JSDebugVariable *closures;
    int closure_count;
    const JSDebugScope *scopes;
    int scope_count;
} JSDebugFunctionInfo;

typedef void JSDebugInfoCollector(JSContext *ctx, const JSDebugFunctionInfo *info,
                                  void *opaque);

/* Sets the collector (NULL: none). */
void JS_SetDebugInfoCollector(JSRuntime *rt, JSDebugInfoCollector *collector,
                              void *opaque);

/* Compile warnings: what a program asks of the compiler when it runs, which
   an engine built without compiler (QJSX_NO_COMPILER: the devices) refuses
   with "eval is not supported". The handler is called while a source is
   compiled, with the position of the construct (line and column from 1, as
   in a SyntaxError).

   The detection is syntactic: a direct call eval(...), and a call or a
   'new' of the name Function. An indirect use ((0, eval)(x),
   globalThis.eval, an alias) is not seen; a variable of the program that
   bears one of these names is reported too. */
typedef enum {
    JS_COMPILE_WARNING_EVAL = 1,      /* eval(...) */
    JS_COMPILE_WARNING_FUNCTION = 2,  /* Function(...), new Function(...) */
} JSCompileWarningKind;

typedef void JSCompileWarningHandler(JSContext *ctx, JSCompileWarningKind kind,
                                     int line, int column, void *opaque);

/* Sets the handler (NULL: none). */
void JS_SetCompileWarningHandler(JSRuntime *rt, JSCompileWarningHandler *handler,
                                 void *opaque);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* QUICKJS_DEBUG_H */
