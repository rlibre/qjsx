/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/*
 * QuickJS - compiler/debug-info.c
 *
 * qjsx: debug info of the compiled functions, given to the collector set by
 * JS_SetDebugInfoCollector() (quickjs-debug.h). Not part of upstream
 * QuickJS: see doc/engine-changes.md.
 *
 * Positions are recorded by resolve_labels() while it emits the final
 * bytecode (debugger positions carried by OP_line_num, and the positions of
 * the pc2line table); variables and scopes are read from the JSFunctionDef
 * by js_create_function() before it strips them.
 */

#include "quickjs-internal.h"
#include "compiler/compiler.h"

void JS_SetDebugInfoCollector(JSRuntime *rt, JSDebugInfoCollector *collector,
                              void *opaque)
{
    rt->debug.info_collector = collector;
    rt->debug.info_opaque = opaque;
}

void JS_SetCompileWarningHandler(JSRuntime *rt, JSCompileWarningHandler *handler,
                                 void *opaque)
{
    rt->debug.warning_handler = handler;
    rt->debug.warning_opaque = opaque;
}

/* Tells the handler, if any, about a construct that needs the compiler at
   run time. 'ptr' is its position in the source. */
void js_compile_warning(JSParseState *s, JSCompileWarningKind kind,
                        const uint8_t *ptr)
{
    JSRuntime *rt = s->ctx->rt;
    int line_num, col_num;

    if (!rt->debug.warning_handler)
        return;
    line_num = get_line_col(&col_num, s->buf_start, ptr - s->buf_start);
    rt->debug.warning_handler(s->ctx, kind, line_num + 1, col_num + 1,
                              rt->debug.warning_opaque);
}

/* TRUE if the last instruction emitted reads the variable 'name' by its
   name: the callee of a call that follows is that name. */
BOOL js_last_op_is_scope_var(JSFunctionDef *fd, JSAtom name)
{
    if (get_prev_opcode(fd) != OP_scope_get_var)
        return FALSE;
    return get_u32(fd->byte_code.buf + fd->last_opcode_pos + 1) == name;
}

static BOOL js_debug_info_wanted(JSFunctionDef *s)
{
    return s->ctx->rt->debug.info_collector != NULL;
}

/* 'operand' is an OP_line_num operand: source offset + kind. */
void js_debug_info_add_position(JSFunctionDef *s, uint32_t pc, uint32_t operand)
{
    JSDebugPosition position;
    JSDebugPosition *last;

    if (!js_debug_info_wanted(s))
        return;
    position.pc = pc;
    position.source_pos = line_num_pos(operand);
    position.kind = line_num_kind(operand);

    if (s->debug_position_count > 0) {
        last = &s->debug_positions[s->debug_position_count - 1];
        BOOL same_pc = last->pc == pc;
        BOOL both_stops = last->kind != JS_DEBUG_POSITION_EXPRESSION &&
                          position.kind != JS_DEBUG_POSITION_EXPRESSION;
        /* two stops at the same pc: the code of the first one is empty
           (e.g. a block start followed by its first statement): the last
           one wins */
        if (same_pc && both_stops) {
            *last = position;
            return;
        }
        /* nothing new */
        if (same_pc && last->source_pos == position.source_pos)
            return;
        if (position.kind == JS_DEBUG_POSITION_EXPRESSION &&
            last->source_pos == position.source_pos)
            return;
    }
    if (js_resize_array(s->ctx, (void **)&s->debug_positions,
                        sizeof(s->debug_positions[0]),
                        &s->debug_position_size, s->debug_position_count + 1)) {
        /* out of memory: the debug info is incomplete, the bytecode is
           not affected */
        JS_FreeValue(s->ctx, JS_GetException(s->ctx));
        return;
    }
    s->debug_positions[s->debug_position_count++] = position;
}

/* The peephole optimizations of resolve_variables() and resolve_labels()
   skip the OP_line_num between the instructions they merge. When a pass
   jumps from 'start' to 'end' in its input, this returns the last debugger
   position (statement or return) in between, to put on the next emitted
   instruction, or 0 if none.
   A jump that ends on a label (or at the end) is a dead code removal
   (skip_dead_code): the positions of dead code are dropped. Likewise, a
   position followed by a label in the range was in dead code. */
uint32_t js_debug_skipped_marker(const uint8_t *bc_buf, int bc_len, int start, int end)
{
    uint32_t marker = 0;
    int pos = start;

    if (end >= bc_len || bc_buf[end] == OP_label)
        return 0;
    while (pos < end) {
        int op = bc_buf[pos];
        if (op == OP_line_num) {
            uint32_t operand = get_u32(bc_buf + pos + 1);
            if (line_num_kind(operand) != JS_DEBUG_POSITION_EXPRESSION)
                marker = operand;
        } else if (op == OP_label) {
            marker = 0;
        }
        pos += opcode_info[op].size;
    }
    return marker;
}

static int js_debug_variable_flags(const JSVarDef *vd)
{
    int flags = 0;
    if (vd->is_lexical)
        flags |= JS_DEBUG_VARIABLE_LEXICAL;
    if (vd->is_const)
        flags |= JS_DEBUG_VARIABLE_CONST;
    if (vd->is_captured)
        flags |= JS_DEBUG_VARIABLE_CAPTURED;
    return flags;
}

/* TRUE for a reference to a global variable (not a variable of an
   enclosing function or of the module) */
static BOOL js_debug_closure_is_global(const JSClosureVar *cv)
{
    switch (cv->closure_type) {
    case JS_CLOSURE_GLOBAL_REF:
    case JS_CLOSURE_GLOBAL_DECL:
    case JS_CLOSURE_GLOBAL:
        return TRUE;
    default:
        return FALSE;
    }
}

/* Calls the collector for the function being created ('b' has its
   bytecode and number; the variables are still those of 'fd'). */
void js_debug_info_collect(JSContext *ctx, JSFunctionDef *fd, JSFunctionBytecode *b)
{
    JSRuntime *rt = ctx->rt;
    JSDebugFunctionInfo info;
    JSDebugVariable *arguments = NULL;
    JSDebugVariable *locals = NULL;
    JSDebugVariable *closures = NULL;
    JSDebugScope *scopes = NULL;
    int i;

    if (rt->debug.info_collector == NULL)
        return;

    memset(&info, 0, sizeof(info));
    info.function_number = b->function_number;
    info.name = fd->func_name;
    info.source_start = fd->source_pos;
    /* functions without known end (generated ones) end where they start */
    info.source_end = fd->source_end > fd->source_pos ? fd->source_end : fd->source_pos;
    info.bytecode_length = b->byte_code_len;
    info.positions = fd->debug_positions;
    info.position_count = fd->debug_position_count;

    arguments = js_mallocz(ctx, sizeof(arguments[0]) * max_int(fd->arg_count, 1));
    locals = js_mallocz(ctx, sizeof(locals[0]) * max_int(fd->var_count, 1));
    closures = js_mallocz(ctx, sizeof(closures[0]) * max_int(fd->closure_var_count, 1));
    scopes = js_mallocz(ctx, sizeof(scopes[0]) * max_int(fd->scope_count, 1));
    if (!arguments || !locals || !closures || !scopes) {
        JS_FreeValue(ctx, JS_GetException(ctx));   /* out of memory: no debug info */
        goto done;
    }

    for (i = 0; i < fd->arg_count; i++) {
        arguments[i].name = fd->args[i].var_name;
        arguments[i].scope = -1;
        arguments[i].flags = js_debug_variable_flags(&fd->args[i]);
    }
    for (i = 0; i < fd->var_count; i++) {
        locals[i].name = fd->vars[i].var_name;
        locals[i].scope = fd->vars[i].scope_level;
        locals[i].flags = js_debug_variable_flags(&fd->vars[i]);
    }
    for (i = 0; i < fd->closure_var_count; i++) {
        closures[i].name = fd->closure_var[i].var_name;
        closures[i].scope = -1;
        closures[i].flags = 0;
        if (fd->closure_var[i].is_lexical)
            closures[i].flags |= JS_DEBUG_VARIABLE_LEXICAL;
        if (fd->closure_var[i].is_const)
            closures[i].flags |= JS_DEBUG_VARIABLE_CONST;
        if (js_debug_closure_is_global(&fd->closure_var[i]))
            closures[i].flags |= JS_DEBUG_VARIABLE_GLOBAL;
    }
    for (i = 0; i < fd->scope_count; i++) {
        const JSVarScope *scope = &fd->scopes[i];
        scopes[i].parent = scope->parent;
        scopes[i].source_start = scope->source_start >= 0 ? (uint32_t)scope->source_start : info.source_start;
        scopes[i].source_end = scope->source_end >= 0 ? (uint32_t)scope->source_end : info.source_end;
    }

    info.arguments = arguments;
    info.argument_count = fd->arg_count;
    info.locals = locals;
    info.local_count = fd->var_count;
    info.closures = closures;
    info.closure_count = fd->closure_var_count;
    info.scopes = scopes;
    info.scope_count = fd->scope_count;
    rt->debug.info_collector(ctx, &info, rt->debug.info_opaque);

 done:
    js_free(ctx, arguments);
    js_free(ctx, locals);
    js_free(ctx, closures);
    js_free(ctx, scopes);
}
