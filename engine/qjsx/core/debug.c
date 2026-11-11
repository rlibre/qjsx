/*
 * QuickJS - core/debug.c
 *
 * espjs: debugger support (not part of upstream QuickJS). Public API in
 * quickjs-debug.h; the interpreter calls the js_debug_* functions below
 * (core/interpreter.c). See doc/engine-changes.md.
 */

#include "quickjs-internal.h"

/* ---------------------------------------------------------------------- */
/* Configuration */

void JS_SetDebugHook(JSRuntime *rt, JSDebugHook *hook, void *opaque)
{
    rt->debug.hook = hook;
    rt->debug.hook_opaque = opaque;
    if (hook == NULL) {
        js_debug_forget_exception(rt);
    }
}

void JS_DebugSetStepping(JSRuntime *rt, JS_BOOL stepping)
{
    rt->debug.stepping = stepping != 0;
}

void JS_DebugSetExceptionReporting(JSRuntime *rt, JS_BOOL report)
{
    rt->debug.report_exceptions = report != 0;
    if (!report) {
        js_debug_forget_exception(rt);
    }
}

/* ---------------------------------------------------------------------- */
/* Hook calls from the interpreter */

static BOOL js_debug_hook_callable(JSRuntime *rt)
{
    BOOL hook_installed = rt->debug.hook != NULL;
    BOOL hook_running = rt->debug.paused_frame != NULL;
    return hook_installed && !hook_running;
}

static void js_debug_init_event(JSDebugEvent *event, JSDebugReason reason)
{
    event->reason = reason;
    event->exception = JS_UNDEFINED;
    event->at_return = FALSE;
    event->return_value = JS_UNDEFINED;
}

/* 'pc' is the instruction where the frame 'sf' is stopped. */
static int js_debug_call_hook(JSContext *ctx, JSStackFrame *sf, const uint8_t *pc,
                              const JSDebugEvent *event)
{
    JSRuntime *rt = ctx->rt;
    int result;

    /* the frame API reads the position of the frame from cur_pc */
    sf->cur_pc = pc;
    rt->debug.paused_frame = sf;
    result = rt->debug.hook(ctx, event, rt->debug.hook_opaque);
    rt->debug.paused_frame = NULL;
    return result;
}

static BOOL bitmap_has(const uint8_t *bitmap, uint32_t offset)
{
    if (bitmap == NULL)
        return FALSE;
    return (bitmap[offset >> 3] >> (offset & 7)) & 1;
}

/* Fills the return value of 'event' when the frame stops on a return
   instruction. 'sp' is the stack pointer of the frame. */
static void js_debug_set_return_value(JSDebugEvent *event, const uint8_t *pc,
                                      const JSValue *sp)
{
    switch (*pc) {
    case OP_return:
    case OP_return_async:
        event->at_return = TRUE;
        event->return_value = sp[-1];
        break;
    case OP_return_undef:
        event->at_return = TRUE;
        event->return_value = JS_UNDEFINED;
        break;
    default:
        break;
    }
}

int js_debug_check_stop(JSContext *ctx, JSStackFrame *sf, JSFunctionBytecode *b,
                        const uint8_t *pc, const JSValue *sp)
{
    JSRuntime *rt = ctx->rt;
    uint32_t offset = pc - b->byte_code_buf;
    JSDebugEvent event;

    if (!js_debug_hook_callable(rt))
        return 0;
    if (bitmap_has(b->debug_breakpoints, offset)) {
        js_debug_init_event(&event, JS_DEBUG_REASON_BREAKPOINT);
    } else if (rt->debug.stepping && bitmap_has(b->debug_statements, offset)) {
        js_debug_init_event(&event, JS_DEBUG_REASON_STEP);
    } else {
        return 0;
    }
    js_debug_set_return_value(&event, pc, sp);
    return js_debug_call_hook(ctx, sf, pc, &event);
}

int js_debug_on_debugger_statement(JSContext *ctx, JSStackFrame *sf, const uint8_t *pc)
{
    JSDebugEvent event;

    if (!js_debug_hook_callable(ctx->rt))
        return 0;
    js_debug_init_event(&event, JS_DEBUG_REASON_STATEMENT);
    return js_debug_call_hook(ctx, sf, pc, &event);
}

/* ---------------------------------------------------------------------- */
/* Exceptions

   The interpreter goes through its 'exception' label in the frame of the
   throw, then again in each calling frame while the exception unwinds. The
   value given to the hook is remembered (rt->debug.reported_exception) so
   that only the first pass reports it. It is forgotten when a 'catch'
   clause catches it or when C code takes it (JS_GetException): throwing it
   again is then a new throw. */

void js_debug_forget_exception(JSRuntime *rt)
{
    JS_FreeValueRT(rt, rt->debug.reported_exception);
    rt->debug.reported_exception = JS_UNINITIALIZED;
}

/* Remembers the pending exception as already reported. */
void js_debug_mark_exception_reported(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    JSValue pending = rt->current_exception;

    if (rt->debug.hook == NULL)
        return;
    JS_FreeValue(ctx, rt->debug.reported_exception);
    rt->debug.reported_exception = JS_DupValue(ctx, pending);
}

void js_debug_on_exception(JSContext *ctx, JSStackFrame *sf, const uint8_t *pc)
{
    JSRuntime *rt = ctx->rt;
    JSValue thrown;
    JSDebugEvent event;

    if (!rt->debug.report_exceptions || !js_debug_hook_callable(rt))
        return;
    /* interruptions (JS_ThrowInterrupted) are not JS throws */
    if (rt->current_exception_is_uncatchable)
        return;
    if (js_same_value(ctx, rt->debug.reported_exception, rt->current_exception))
        return;

    /* the hook runs without pending exception, so that it can call JS */
    thrown = rt->current_exception;
    rt->current_exception = JS_UNINITIALIZED;
    js_debug_init_event(&event, JS_DEBUG_REASON_EXCEPTION);
    event.exception = thrown;
    js_debug_call_hook(ctx, sf, pc, &event);
    /* an exception left by the hook is dropped: the thrown value goes on */
    JS_FreeValue(ctx, rt->current_exception);
    rt->current_exception = thrown;
    /* marked after the hook: JS_GetException calls made by the hook (for
       its own evaluations) forget the reported value */
    js_debug_mark_exception_reported(ctx);
}

/* The compiler emits "gosub <finally>; throw" as the handler of the try
   block of a try/finally, and of the catch block of a try/catch: the value
   is caught, the finally clause runs, then the same value is thrown again.
   When the finally clause is empty, resolve_labels() removes the gosub and
   the handler is a plain "throw". */
static BOOL js_debug_is_rethrow_handler(JSFunctionBytecode *b, int handler_offset)
{
    int gosub_size = opcode_info[OP_gosub].size;
    const uint8_t *handler = b->byte_code_buf + handler_offset;
    BOOL plain_rethrow = handler[0] == OP_throw;
    BOOL finally_rethrow = handler_offset + gosub_size < b->byte_code_len &&
                           handler[0] == OP_gosub && handler[gosub_size] == OP_throw;

    return plain_rethrow || finally_rethrow;
}

void js_debug_exception_caught(JSRuntime *rt, JSFunctionBytecode *b, int handler_offset)
{
    if (JS_IsUninitialized(rt->debug.reported_exception))
        return;
    if (js_debug_is_rethrow_handler(b, handler_offset))
        return;
    js_debug_forget_exception(rt);
}

/* ---------------------------------------------------------------------- */
/* Stops */

/* Function 'number' in the tree of functions whose root is 'b' (the
   functions defined inside a function are in its constant pool). */
static JSFunctionBytecode *js_debug_find_in_tree(JSFunctionBytecode *b, int number)
{
    int i;

    if (b->function_number == number)
        return b;
    for (i = 0; i < b->cpool_count; i++) {
        JSValue constant = b->cpool[i];
        if (JS_VALUE_GET_TAG(constant) == JS_TAG_FUNCTION_BYTECODE) {
            JSFunctionBytecode *child = JS_VALUE_GET_PTR(constant);
            JSFunctionBytecode *found = js_debug_find_in_tree(child, number);
            if (found != NULL)
                return found;
        }
    }
    return NULL;
}

/* Top-level function of a module: its bytecode until the module function
   is created, then the function object. */
static JSFunctionBytecode *js_debug_module_bytecode(JSModuleDef *m)
{
    JSValue function = m->func_obj;

    if (JS_VALUE_GET_TAG(function) == JS_TAG_FUNCTION_BYTECODE)
        return JS_VALUE_GET_PTR(function);
    if (JS_VALUE_GET_TAG(function) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(function);
        if (p->class_id == JS_CLASS_BYTECODE_FUNCTION)
            return p->u.func.function_bytecode;
    }
    return NULL;
}

static JSFunctionBytecode *js_debug_find_function(JSContext *ctx, const char *module_name,
                                                  int function_number)
{
    JSAtom name = JS_NewAtom(ctx, module_name);
    JSFunctionBytecode *found = NULL;
    struct list_head *el;

    if (name == JS_ATOM_NULL) {
        JS_FreeValue(ctx, JS_GetException(ctx));   /* out of memory */
        return NULL;
    }
    list_for_each(el, &ctx->loaded_modules) {
        JSModuleDef *m = list_entry(el, JSModuleDef, link);
        JSFunctionBytecode *root;
        if (m->module_name != name)
            continue;
        root = js_debug_module_bytecode(m);
        if (root != NULL)
            found = js_debug_find_in_tree(root, function_number);
        break;
    }
    JS_FreeAtom(ctx, name);
    return found;
}

int JS_DebugSetStops(JSContext *ctx, const char *module_name, int function_number,
                     JSDebugStopKind kind, const uint32_t *offsets, int count)
{
    JSFunctionBytecode *b = js_debug_find_function(ctx, module_name, function_number);
    uint8_t **slot;
    uint8_t *bitmap = NULL;
    int i;

    if (b == NULL)
        return -1;
    for (i = 0; i < count; i++) {
        if (offsets[i] >= (uint32_t)b->byte_code_len)
            return -1;
    }
    if (count > 0) {
        size_t bitmap_size = (b->byte_code_len + 7) / 8;
        bitmap = js_mallocz_rt(ctx->rt, bitmap_size);
        if (bitmap == NULL)
            return -1;
        for (i = 0; i < count; i++) {
            uint32_t offset = offsets[i];
            bitmap[offset >> 3] |= 1 << (offset & 7);
        }
    }
    slot = (kind == JS_DEBUG_STOPS_BREAKPOINTS) ? &b->debug_breakpoints : &b->debug_statements;
    js_free_rt(ctx->rt, *slot);
    *slot = bitmap;
    return 0;
}

static void js_debug_clear_tree_stops(JSRuntime *rt, JSFunctionBytecode *b)
{
    int i;

    js_free_rt(rt, b->debug_breakpoints);
    js_free_rt(rt, b->debug_statements);
    b->debug_breakpoints = NULL;
    b->debug_statements = NULL;
    for (i = 0; i < b->cpool_count; i++) {
        JSValue constant = b->cpool[i];
        if (JS_VALUE_GET_TAG(constant) == JS_TAG_FUNCTION_BYTECODE)
            js_debug_clear_tree_stops(rt, JS_VALUE_GET_PTR(constant));
    }
}

void JS_DebugClearAllStops(JSContext *ctx)
{
    struct list_head *el;

    list_for_each(el, &ctx->loaded_modules) {
        JSModuleDef *m = list_entry(el, JSModuleDef, link);
        JSFunctionBytecode *root = js_debug_module_bytecode(m);
        if (root != NULL)
            js_debug_clear_tree_stops(ctx->rt, root);
    }
}

/* ---------------------------------------------------------------------- */
/* Frames and slots */

static JSStackFrame *js_debug_frame_at(JSRuntime *rt, int level)
{
    JSStackFrame *sf = rt->debug.paused_frame;
    int i;

    if (level < 0)
        return NULL;
    for (i = 0; sf != NULL && i < level; i++)
        sf = sf->prev_frame;
    return sf;
}

/* Bytecode of the function running in 'sf', NULL for a native function. */
static JSFunctionBytecode *js_debug_frame_bytecode(JSStackFrame *sf)
{
    JSObject *p;

    if (JS_VALUE_GET_TAG(sf->cur_func) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(sf->cur_func);
    if (p->class_id != JS_CLASS_BYTECODE_FUNCTION)
        return NULL;
    return p->u.func.function_bytecode;
}

int JS_DebugGetFunctionLocation(JSContext *ctx, JSValueConst function,
                                JSAtom *module_name, int *function_number)
{
    JSObject *p;
    JSFunctionBytecode *b;

    if (JS_VALUE_GET_TAG(function) != JS_TAG_OBJECT)
        return -1;
    p = JS_VALUE_GET_OBJ(function);
    if (p->class_id != JS_CLASS_BYTECODE_FUNCTION)
        return -1;
    b = p->u.func.function_bytecode;
    *module_name = b->filename;
    *function_number = b->function_number;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Objects */

static JSDebugObjectKind js_debug_object_kind(JSContext *ctx, JSObject *p)
{
    switch (p->class_id) {
    case JS_CLASS_ARRAY:
    case JS_CLASS_ARGUMENTS:
    case JS_CLASS_MAPPED_ARGUMENTS:
        return JS_DEBUG_OBJECT_ARRAY;
    case JS_CLASS_ERROR:
        return JS_DEBUG_OBJECT_ERROR;
    case JS_CLASS_DATE:
        return JS_DEBUG_OBJECT_DATE;
    case JS_CLASS_REGEXP:
        return JS_DEBUG_OBJECT_REGEXP;
    case JS_CLASS_MAP:
        return JS_DEBUG_OBJECT_MAP;
    case JS_CLASS_SET:
        return JS_DEBUG_OBJECT_SET;
    case JS_CLASS_WEAKMAP:
        return JS_DEBUG_OBJECT_WEAKMAP;
    case JS_CLASS_WEAKSET:
        return JS_DEBUG_OBJECT_WEAKSET;
    case JS_CLASS_PROMISE:
        return JS_DEBUG_OBJECT_PROMISE;
    case JS_CLASS_PROXY:
        return JS_DEBUG_OBJECT_PROXY;
    case JS_CLASS_ARRAY_BUFFER:
    case JS_CLASS_SHARED_ARRAY_BUFFER:
        return JS_DEBUG_OBJECT_ARRAY_BUFFER;
    case JS_CLASS_DATAVIEW:
        return JS_DEBUG_OBJECT_DATAVIEW;
    case JS_CLASS_GENERATOR:
    case JS_CLASS_ASYNC_GENERATOR:
        return JS_DEBUG_OBJECT_GENERATOR;
    case JS_CLASS_ITERATOR:
    case JS_CLASS_ITERATOR_CONCAT:
    case JS_CLASS_ITERATOR_HELPER:
    case JS_CLASS_ITERATOR_WRAP:
    case JS_CLASS_MAP_ITERATOR:
    case JS_CLASS_SET_ITERATOR:
    case JS_CLASS_ARRAY_ITERATOR:
    case JS_CLASS_STRING_ITERATOR:
    case JS_CLASS_REGEXP_STRING_ITERATOR:
        return JS_DEBUG_OBJECT_ITERATOR;
    case JS_CLASS_NUMBER:
    case JS_CLASS_STRING:
    case JS_CLASS_BOOLEAN:
    case JS_CLASS_SYMBOL:
    case JS_CLASS_BIG_INT:
        return JS_DEBUG_OBJECT_WRAPPER;
    case JS_CLASS_WEAK_REF:
        return JS_DEBUG_OBJECT_WEAKREF;
    default:
        break;
    }
    if (p->class_id >= JS_CLASS_UINT8C_ARRAY && p->class_id <= JS_CLASS_FLOAT64_ARRAY)
        return JS_DEBUG_OBJECT_TYPED_ARRAY;
    /* any callable object: functions of every kind, native classes with
       a call method */
    if (JS_IsFunction(ctx, JS_MKPTR(JS_TAG_OBJECT, p)))
        return JS_DEBUG_OBJECT_FUNCTION;
    return JS_DEBUG_OBJECT_PLAIN;
}

static int64_t js_debug_object_size(JSObject *p, JSDebugObjectKind kind)
{
    switch (kind) {
    case JS_DEBUG_OBJECT_MAP:
    case JS_DEBUG_OBJECT_SET:
        return p->u.map_state->record_count;
    case JS_DEBUG_OBJECT_TYPED_ARRAY:
        return p->u.array.count;
    case JS_DEBUG_OBJECT_ARRAY_BUFFER:
        return p->u.array_buffer->byte_length;
    default:
        return -1;
    }
}

int JS_DebugGetObjectInfo(JSContext *ctx, JSValueConst obj, JSDebugObjectInfo *info)
{
    JSObject *p;

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return -1;
    p = JS_VALUE_GET_OBJ(obj);
    info->kind = js_debug_object_kind(ctx, p);
    info->class_name = ctx->rt->class_array[p->class_id].class_name;
    info->size = js_debug_object_size(p, info->kind);
    return 0;
}

JSValue JS_DebugGetInternal(JSContext *ctx, JSValueConst obj, JSDebugInternal which)
{
    JSObject *p;
    JSDebugObjectKind kind;

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return JS_UNDEFINED;
    p = JS_VALUE_GET_OBJ(obj);
    kind = js_debug_object_kind(ctx, p);
    switch (which) {
    case JS_DEBUG_INTERNAL_PROXY_TARGET:
        if (kind != JS_DEBUG_OBJECT_PROXY)
            return JS_UNDEFINED;
        return JS_DupValue(ctx, p->u.proxy_data->target);
    case JS_DEBUG_INTERNAL_PROXY_HANDLER:
        if (kind != JS_DEBUG_OBJECT_PROXY)
            return JS_UNDEFINED;
        return JS_DupValue(ctx, p->u.proxy_data->handler);
    case JS_DEBUG_INTERNAL_PRIMITIVE_VALUE:
        if (kind != JS_DEBUG_OBJECT_WRAPPER && kind != JS_DEBUG_OBJECT_DATE)
            return JS_UNDEFINED;
        return JS_DupValue(ctx, p->u.object_data);
    case JS_DEBUG_INTERNAL_BOUND_TARGET:
        if (p->class_id != JS_CLASS_BOUND_FUNCTION)
            return JS_UNDEFINED;
        return JS_DupValue(ctx, p->u.bound_function->func_obj);
    }
    return JS_UNDEFINED;
}

int JS_DebugGetFrame(JSContext *ctx, int level, JSDebugFrame *frame)
{
    JSStackFrame *sf = js_debug_frame_at(ctx->rt, level);
    JSFunctionBytecode *b;

    if (sf == NULL)
        return -1;
    memset(frame, 0, sizeof(*frame));
    frame->id = (uintptr_t)sf;
    b = js_debug_frame_bytecode(sf);
    if (b == NULL)
        return 0;
    frame->is_bytecode = TRUE;
    frame->module_name = b->filename;
    frame->function_number = b->function_number;
    frame->pc = sf->cur_pc - b->byte_code_buf;
    frame->argument_count = sf->arg_count;
    frame->local_count = b->var_count;
    frame->closure_count = b->closure_var_count;
    return 0;
}

JSValue JS_DebugGetFrameFunction(JSContext *ctx, int level)
{
    JSStackFrame *sf = js_debug_frame_at(ctx->rt, level);

    if (sf == NULL)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, sf->cur_func);
}

/* Address of a slot of the bytecode frame at 'level', NULL if none. */
static JSValue *js_debug_slot(JSContext *ctx, int level, JSDebugSlotKind kind, int index)
{
    JSStackFrame *sf = js_debug_frame_at(ctx->rt, level);
    JSFunctionBytecode *b;
    JSObject *p;
    JSVarRef *ref;

    if (sf == NULL || index < 0)
        return NULL;
    b = js_debug_frame_bytecode(sf);
    if (b == NULL)
        return NULL;
    switch (kind) {
    case JS_DEBUG_SLOT_ARGUMENT:
        if (index >= sf->arg_count)
            return NULL;
        return &sf->arg_buf[index];
    case JS_DEBUG_SLOT_LOCAL:
        if (index >= b->var_count)
            return NULL;
        return &sf->var_buf[index];
    case JS_DEBUG_SLOT_CLOSURE:
        if (index >= b->closure_var_count)
            return NULL;
        p = JS_VALUE_GET_OBJ(sf->cur_func);
        ref = p->u.func.var_refs[index];
        if (ref == NULL)
            return NULL;
        return ref->pvalue;
    }
    return NULL;
}

JSValue JS_DebugGetSlot(JSContext *ctx, int level, JSDebugSlotKind kind, int index)
{
    JSValue *slot = js_debug_slot(ctx, level, kind, index);

    if (slot == NULL)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, *slot);
}

int JS_DebugSetSlot(JSContext *ctx, int level, JSDebugSlotKind kind, int index, JSValue value)
{
    JSValue *slot = js_debug_slot(ctx, level, kind, index);

    if (slot == NULL) {
        JS_FreeValue(ctx, value);
        return -1;
    }
    set_value(ctx, slot, value);
    return 0;
}
