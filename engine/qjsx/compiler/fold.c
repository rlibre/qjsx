/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/*
 * QuickJS - compiler/fold.c
 *
 * qjsx: computes at compile time the expressions made of literal numbers and
 * strings only ("constant folding"). Not part of upstream QuickJS, which
 * only folds the minus sign of a number: see doc/engine-changes.md (R).
 *
 *     5 * 1000           push_i32 5; push_i32 1000; mul    ->  push_i32 5000
 *     "ab" + "cd"        push_atom_value x 2; add           ->  push_atom_value "abcd"
 *     "v" + 1 + 2        ...                                ->  push_atom_value "v12"
 *
 * The pass runs between resolve_variables() and resolve_labels(), on the
 * bytecode with symbolic labels. It reads the instructions in order and
 * keeps aside the last constants pushed, not written yet: they are the top
 * of the stack at run time.
 *   - a constant push (OP_push_i32, OP_push_const of a number or a string,
 *     OP_push_atom_value) is kept aside;
 *   - an operator whose operands are all kept aside and that fold_binary()
 *     or fold_unary() can compute is replaced, with its operands, by its
 *     result, kept aside in turn (so "1 + 2 + 3" folds twice);
 *   - any other instruction, a label included (the stack is then not the one
 *     of the pushes before it), first writes what was kept aside, unchanged
 *     and in the same order.
 * The positions of the debugger and of the errors (OP_line_num) met while
 * constants are kept aside keep their place among them.
 *
 * The values are computed as the interpreter does: numbers as doubles (the
 * result is an integer instruction when it is a 32 bit integer, -0
 * excepted), shifts and bit operations on 32 bit integers, strings joined
 * after the conversion of a number by the engine itself. Left to run time:
 * bigints, booleans, null, undefined, comparisons, and strings holding half
 * of a surrogate pair.
 */

#include "quickjs-internal.h"
#include "compiler/compiler.h"

/* constants and positions kept aside at most: more are written at once */
#define FOLD_MAX_ITEMS 32
/* a joined string longer than this is left to run time */
#define FOLD_MAX_STRING 65536

/* What is kept aside, in the order of the input. */
typedef struct FoldItem {
    BOOL is_marker;   /* an OP_line_num, else a constant */
    BOOL is_new;      /* a computed constant: no instruction of the input */
    int pos;          /* its instruction in the input (not for a computed constant) */
    JSValue value;    /* the constant, owned: a number or a string */
} FoldItem;

typedef struct FoldState {
    JSContext *ctx;
    JSFunctionDef *fd;
    const uint8_t *buf;               /* the input */
    DynBuf out;                       /* the output */
    FoldItem items[FOLD_MAX_ITEMS];
    int item_count;
    int folded;                       /* operators computed */
} FoldState;

/* ### Constants ############################################################# */

static BOOL fold_is_number(JSValueConst v)
{
    int tag = JS_VALUE_GET_TAG(v);
    return tag == JS_TAG_INT || tag == JS_TAG_FLOAT64;
}

static double fold_number(JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
        return JS_VALUE_GET_INT(v);
    return JS_VALUE_GET_FLOAT64(v);
}

/* a flat string without half of a surrogate pair: its UTF-8 form gives the
   same string back */
static BOOL fold_is_string(JSValueConst v)
{
    JSString *p;
    uint32_t i;

    if (JS_VALUE_GET_TAG(v) != JS_TAG_STRING)
        return FALSE;
    p = JS_VALUE_GET_STRING(v);
    if (p->is_wide_char) {
        for(i = 0; i < p->len; i++) {
            if (p->u.str16[i] >= 0xd800 && p->u.str16[i] <= 0xdfff)
                return FALSE;
        }
    }
    return TRUE;
}

/* The constant pushed by the instruction at 'pos' (owned by the caller), or
   JS_UNINITIALIZED when it is not a number or a string constant. */
static JSValue fold_constant_at(JSContext *ctx, JSFunctionDef *fd, const uint8_t *buf, int pos)
{
    JSValueConst v;

    switch(buf[pos]) {
    case OP_push_i32:
        return JS_NewInt32(ctx, (int32_t)get_u32(buf + pos + 1));
    case OP_push_const:
        v = fd->cpool[get_u32(buf + pos + 1)];
        if (fold_is_number(v) || JS_VALUE_GET_TAG(v) == JS_TAG_STRING)
            return JS_DupValue(ctx, v);
        return JS_UNINITIALIZED;
    case OP_push_atom_value:
        return JS_AtomToString(ctx, get_u32(buf + pos + 1));
    default:
        return JS_UNINITIALIZED;
    }
}

/* ### Operators ############################################################# */

/* 'a' + 'b' where one at least is a string: JS_UNINITIALIZED when left to
   run time, JS_EXCEPTION out of memory. */
static JSValue fold_concat(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    const char *text_a, *text_b;
    size_t len_a, len_b;
    char *joined;
    JSValue result = JS_UNINITIALIZED;

    /* each operand: a string we can join, or a number */
    if (!(fold_is_string(a) || fold_is_number(a)) || !(fold_is_string(b) || fold_is_number(b)))
        return JS_UNINITIALIZED;

    /* their text (the engine converts the numbers) */
    text_a = JS_ToCStringLen(ctx, &len_a, a);
    text_b = JS_ToCStringLen(ctx, &len_b, b);
    if (!text_a || !text_b) {
        result = JS_EXCEPTION;
        goto done;
    }
    if (len_a + len_b > FOLD_MAX_STRING)
        goto done;

    /* joined */
    joined = js_malloc(ctx, len_a + len_b + 1);
    if (!joined) {
        result = JS_EXCEPTION;
        goto done;
    }
    memcpy(joined, text_a, len_a);
    memcpy(joined + len_a, text_b, len_b);
    result = JS_NewStringLen(ctx, joined, len_a + len_b);
    js_free(ctx, joined);

 done:
    JS_FreeCString(ctx, text_a);
    JS_FreeCString(ctx, text_b);
    return result;
}

/* 'a' op 'b': JS_UNINITIALIZED when left to run time, JS_EXCEPTION out of
   memory. */
static JSValue fold_binary(JSContext *ctx, int op, JSValueConst a, JSValueConst b)
{
    double da, db, d;
    int32_t ia, ib;

    /* a string on one side: only '+' */
    if (!fold_is_number(a) || !fold_is_number(b)) {
        if (op != OP_add)
            return JS_UNINITIALIZED;
        return fold_concat(ctx, a, b);
    }

    da = fold_number(a);
    db = fold_number(b);
    switch(op) {
    case OP_add:
        d = da + db;
        break;
    case OP_sub:
        d = da - db;
        break;
    case OP_mul:
        d = da * db;
        break;
    case OP_div:
        d = da / db;
        break;
    case OP_mod:
        d = fmod(da, db);
        break;
    case OP_pow:
        d = js_pow(da, db);
        break;
    default:
        /* shifts and bit operations: on 32 bit integers (neither
           conversion can fail: numbers) */
        JS_ToInt32(ctx, &ia, a);
        JS_ToInt32(ctx, &ib, b);
        switch(op) {
        case OP_shl:
            d = (int32_t)((uint32_t)ia << (ib & 31));
            break;
        case OP_sar:
            d = ia >> (ib & 31);
            break;
        case OP_shr:
            d = (uint32_t)ia >> (ib & 31);
            break;
        case OP_and:
            d = ia & ib;
            break;
        case OP_or:
            d = ia | ib;
            break;
        case OP_xor:
            d = ia ^ ib;
            break;
        default:
            return JS_UNINITIALIZED;
        }
        break;
    }
    return JS_NewFloat64(ctx, d);
}

/* op 'a': JS_UNINITIALIZED when left to run time. */
static JSValue fold_unary(JSContext *ctx, int op, JSValueConst a)
{
    int32_t ia;

    if (!fold_is_number(a))
        return JS_UNINITIALIZED;
    switch(op) {
    case OP_neg:
        return JS_NewFloat64(ctx, -fold_number(a));
    case OP_plus:
        return JS_DupValue(ctx, a);
    case OP_not:
        JS_ToInt32(ctx, &ia, a);
        return JS_NewInt32(ctx, ~ia);
    default:
        return JS_UNINITIALIZED;
    }
}

static BOOL fold_is_binary(int op)
{
    switch(op) {
    case OP_add:
    case OP_sub:
    case OP_mul:
    case OP_div:
    case OP_mod:
    case OP_pow:
    case OP_shl:
    case OP_sar:
    case OP_shr:
    case OP_and:
    case OP_or:
    case OP_xor:
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL fold_is_unary(int op)
{
    return op == OP_neg || op == OP_plus || op == OP_not;
}

/* ### Output ################################################################ */

/* Adds 'val' (taken) to the constants of the function: its index, -1 out of
   memory. */
static int fold_cpool_add(JSContext *ctx, JSFunctionDef *fd, JSValue val)
{
    if (js_resize_array(ctx, (void *)&fd->cpool, sizeof(fd->cpool[0]),
                        &fd->cpool_size, fd->cpool_count + 1)) {
        JS_FreeValue(ctx, val);
        return -1;
    }
    fd->cpool[fd->cpool_count++] = val;
    return fd->cpool_count - 1;
}

/* Writes in 'out', for function 'fd', the push of a constant, as the parser
   writes a literal: an integer, a string by its atom, else a constant of the
   pool. */
static int fold_emit_value(JSContext *ctx, JSFunctionDef *fd, DynBuf *out, JSValueConst value)
{
    int idx;

    if (JS_VALUE_GET_TAG(value) == JS_TAG_INT) {
        dbuf_putc(out, OP_push_i32);
        dbuf_put_u32(out, (uint32_t)JS_VALUE_GET_INT(value));
        return 0;
    }

    if (JS_VALUE_GET_TAG(value) == JS_TAG_STRING) {
        /* warning: JS_NewAtomStr frees the string value */
        JSAtom atom = JS_NewAtomStr(ctx, JS_VALUE_GET_STRING(JS_DupValue(ctx, value)));
        if (atom == JS_ATOM_NULL)
            return -1;
        if (!__JS_AtomIsTaggedInt(atom)) {
            dbuf_putc(out, OP_push_atom_value);
            dbuf_put_u32(out, atom);
            return 0;
        }
    }

    idx = fold_cpool_add(ctx, fd, JS_DupValue(ctx, value));
    if (idx < 0)
        return -1;
    dbuf_putc(out, OP_push_const);
    dbuf_put_u32(out, idx);
    return 0;
}

/* Writes everything kept aside, in order, and forgets it. */
static int fold_flush(FoldState *s)
{
    int i, ret = 0;

    for(i = 0; i < s->item_count; i++) {
        FoldItem *item = &s->items[i];
        if (item->is_marker || !item->is_new) {
            /* an instruction of the input, unchanged */
            dbuf_put(&s->out, s->buf + item->pos, opcode_info[s->buf[item->pos]].size);
        } else if (fold_emit_value(s->ctx, s->fd, &s->out, item->value)) {
            ret = -1;
        }
        if (!item->is_marker)
            JS_FreeValue(s->ctx, item->value);
    }
    s->item_count = 0;
    return ret;
}

/* Index of the last constant kept aside before item 'before', -1 if none. */
static int fold_last_constant(FoldState *s, int before)
{
    int i;

    for(i = before - 1; i >= 0; i--) {
        if (!s->items[i].is_marker)
            return i;
    }
    return -1;
}

/* Forgets the constant kept aside at 'index' (an operand that was computed):
   its value, and the atom its instruction held. */
static void fold_drop_constant(FoldState *s, int index)
{
    FoldItem *item = &s->items[index];

    if (!item->is_new && s->buf[item->pos] == OP_push_atom_value)
        JS_FreeAtom(s->ctx, get_u32(s->buf + item->pos + 1));
    JS_FreeValue(s->ctx, item->value);
    memmove(item, item + 1, (s->item_count - index - 1) * sizeof(*item));
    s->item_count--;
}

/* Keeps aside the instruction at 'pos': a position, or a constant with its
   value (taken). Room is made first by writing what is kept. */
static int fold_keep(FoldState *s, BOOL is_marker, BOOL is_new, int pos, JSValue value)
{
    FoldItem *item;

    if (s->item_count == FOLD_MAX_ITEMS && fold_flush(s)) {
        if (!is_marker)
            JS_FreeValue(s->ctx, value);
        return -1;
    }
    item = &s->items[s->item_count++];
    item->is_marker = is_marker;
    item->is_new = is_new;
    item->pos = pos;
    item->value = value;
    return 0;
}

/* ### Pass ################################################################## */

/* The operator at 'pos' with the constants kept aside: TRUE when it was
   computed (its operands are replaced by the result, kept aside), FALSE when
   it must be written as it is, -1 out of memory. */
static int fold_operator(FoldState *s, int pos)
{
    int op = s->buf[pos];
    int top, below;
    JSValue result;

    /* its operands among what is kept aside */
    top = fold_last_constant(s, s->item_count);
    if (top < 0)
        return FALSE;
    if (fold_is_unary(op)) {
        result = fold_unary(s->ctx, op, s->items[top].value);
        below = -1;
    } else {
        below = fold_last_constant(s, top);
        if (below < 0)
            return FALSE;
        result = fold_binary(s->ctx, op, s->items[below].value, s->items[top].value);
    }
    if (JS_IsException(result))
        return -1;
    if (JS_IsUninitialized(result))
        return FALSE;

    /* the result takes their place (the upper one first: the indexes below
       it do not move) */
    fold_drop_constant(s, top);
    if (below >= 0)
        fold_drop_constant(s, below);
    s->folded++;
    return fold_keep(s, FALSE, TRUE, -1, result) ? -1 : TRUE;
}

int fold_constants(JSContext *ctx, JSFunctionDef *fd)
{
    FoldState state, *s = &state;
    int pos, op, size, len, folded;
    JSValue value;

    s->ctx = ctx;
    s->fd = fd;
    s->buf = fd->byte_code.buf;
    s->item_count = 0;
    s->folded = 0;
    len = fd->byte_code.size;
    js_dbuf_bytecode_init(ctx, &s->out);

    for(pos = 0; pos < len; pos += size) {
        op = s->buf[pos];
        size = opcode_info[op].size;

        /* a position among constants kept aside stays among them */
        if (op == OP_line_num && s->item_count > 0) {
            if (fold_keep(s, TRUE, FALSE, pos, JS_UNDEFINED))
                goto fail;
            continue;
        }

        /* a constant: kept aside */
        value = fold_constant_at(ctx, fd, s->buf, pos);
        if (JS_IsException(value))
            goto fail;
        if (!JS_IsUninitialized(value)) {
            if (fold_keep(s, FALSE, FALSE, pos, value))
                goto fail;
            continue;
        }

        /* an operator on constants: computed */
        if (fold_is_binary(op) || fold_is_unary(op)) {
            folded = fold_operator(s, pos);
            if (folded < 0)
                goto fail;
            if (folded)
                continue;
        }

        /* anything else: what is kept aside, then the instruction; a label
           moves with it */
        if (fold_flush(s))
            goto fail;
        dbuf_put(&s->out, s->buf + pos, size);
        if (op == OP_label)
            fd->label_slots[get_u32(s->buf + pos + 1)].pos2 = s->out.size;
    }
    if (fold_flush(s))
        goto fail;
    if (dbuf_error(&s->out))
        goto fail;

    /* nothing computed: the input stays (the output is the same) */
    if (s->folded == 0) {
        dbuf_free(&s->out);
        return 0;
    }
    dbuf_free(&fd->byte_code);
    fd->byte_code = s->out;
    return 0;

 fail:
    for(pos = 0; pos < s->item_count; pos++) {
        if (!s->items[pos].is_marker)
            JS_FreeValue(ctx, s->items[pos].value);
    }
    dbuf_free(&s->out);
    return -1;
}

/* ### Constants of a module ################################################# */

/*
 * A "const" declared at the top level of a module with a literal value (or
 * an expression of literals and of such constants) is replaced by its value
 * wherever the module reads it: "const DELAY = 5 * 1000;" costs no variable,
 * no entry in the closures of the functions that read it, no checked read.
 *
 * js_find_module_constants() runs on the module function before its child
 * functions are made, on the bytecode of the parser (names not resolved):
 *   1. the candidates: the "const" of the module that are not functions;
 *   2. their values: the top level code is read in order, keeping the last
 *      constants pushed as fold_constants() does; a read of a candidate
 *      whose value is known pushes that value; the initialization of a
 *      candidate (OP_scope_put_var_init) with a constant on top gives it its
 *      value. Any other instruction, a label included, forgets the stack: a
 *      value that depends on a test is not a constant;
 *   3. every function of the module is searched: a candidate that is the
 *      operand of an assignment, a delete or a reference is given up, even
 *      when the name is in fact a local variable of that function (the names
 *      are not resolved yet: to be safe). A declaration of the same name in
 *      a function does not count: it never initializes the module's;
 *   4. a candidate that the module exports keeps its variable, for the
 *      modules that import it; the others lose it (is_removed).
 * A module that calls eval() directly keeps all its variables.
 *
 * resolve_scope_var() then writes the value for each read, and a drop for
 * the initialization of a removed constant.
 *
 * Reading such a constant before its declaration ran gives its value, where
 * QuickJS throws a ReferenceError: agreed (doc/engine-changes.md, R).
 */

/* A candidate of js_find_module_constants(). */
typedef struct ConstCandidate {
    JSAtom name;       /* not owned (the global variable of the module) */
    JSValue value;     /* its value, JS_UNINITIALIZED while unknown; owned */
    int init_count;    /* initializations seen in the top level code */
    BOOL is_rejected;  /* used otherwise than read: not a constant for us */
} ConstCandidate;

typedef struct ConstSearch {
    JSContext *ctx;
    ConstCandidate *candidates;
    int count;
} ConstSearch;

static ConstCandidate *const_find(ConstSearch *search, JSAtom name)
{
    int i;

    for(i = 0; i < search->count; i++) {
        if (search->candidates[i].name == name)
            return &search->candidates[i];
    }
    return NULL;
}

static BOOL const_has_eval(JSFunctionDef *fd)
{
    struct list_head *el;

    if (fd->has_eval_call)
        return TRUE;
    list_for_each(el, &fd->child_list) {
        if (const_has_eval(list_entry(el, JSFunctionDef, link)))
            return TRUE;
    }
    return FALSE;
}

static void const_clear_stack(JSContext *ctx, JSValue *stack, int *pcount)
{
    while (*pcount > 0)
        JS_FreeValue(ctx, stack[--*pcount]);
}

/* Step 2: the values, from the top level code of the module. -1 out of
   memory. */
static int const_find_values(ConstSearch *search, JSFunctionDef *fd)
{
    JSContext *ctx = search->ctx;
    const uint8_t *buf = fd->byte_code.buf;
    int len = fd->byte_code.size;
    JSValue stack[FOLD_MAX_ITEMS], value;
    int count = 0, pos, op, size, ret = -1;
    ConstCandidate *candidate;

    for(pos = 0; pos < len; pos += size) {
        op = buf[pos];
        size = opcode_info[op].size;
        if (op == OP_line_num)
            continue;

        /* a constant, or the read of a candidate whose value is known */
        value = fold_constant_at(ctx, fd, buf, pos);
        if (JS_IsException(value))
            goto done;
        if (JS_IsUninitialized(value) && op == OP_scope_get_var) {
            candidate = const_find(search, get_u32(buf + pos + 1));
            if (candidate && !JS_IsUninitialized(candidate->value))
                value = JS_DupValue(ctx, candidate->value);
        }
        if (!JS_IsUninitialized(value)) {
            if (count == FOLD_MAX_ITEMS)
                const_clear_stack(ctx, stack, &count);
            stack[count++] = value;
            continue;
        }

        /* an operator on the constants on top */
        value = JS_UNINITIALIZED;
        if (fold_is_unary(op) && count >= 1) {
            value = fold_unary(ctx, op, stack[count - 1]);
            if (!JS_IsUninitialized(value) && !JS_IsException(value)) {
                JS_FreeValue(ctx, stack[count - 1]);
                stack[count - 1] = value;
                continue;
            }
        } else if (fold_is_binary(op) && count >= 2) {
            value = fold_binary(ctx, op, stack[count - 2], stack[count - 1]);
            if (!JS_IsUninitialized(value) && !JS_IsException(value)) {
                JS_FreeValue(ctx, stack[--count]);
                JS_FreeValue(ctx, stack[count - 1]);
                stack[count - 1] = value;
                continue;
            }
        }
        if (JS_IsException(value))
            goto done;

        /* the initialization of a candidate: its value if a constant is on
           top; a second one, or one that is not a constant, gives it up */
        if (op == OP_scope_put_var_init) {
            candidate = const_find(search, get_u32(buf + pos + 1));
            if (candidate) {
                candidate->init_count++;
                if (candidate->init_count == 1 && count >= 1)
                    candidate->value = JS_DupValue(ctx, stack[count - 1]);
                else
                    candidate->is_rejected = TRUE;
            }
        }

        /* anything else: the stack is no longer known */
        const_clear_stack(ctx, stack, &count);
    }
    ret = 0;

 done:
    const_clear_stack(ctx, stack, &count);
    return ret;
}

/* Step 3: gives up the candidates that "fd" or a function inside it uses
   otherwise than by reading them. */
static void const_check_uses(ConstSearch *search, JSFunctionDef *fd)
{
    const uint8_t *buf = fd->byte_code.buf;
    int len = fd->byte_code.size;
    int pos, op, size;
    struct list_head *el;
    ConstCandidate *candidate;

    for(pos = 0; pos < len; pos += size) {
        op = buf[pos];
        size = opcode_info[op].size;
        switch(op) {
        case OP_scope_get_var:
        case OP_scope_get_var_undef:
            /* a read */
            break;
        case OP_scope_put_var_init:
            /* the initialization of a declaration: in the top level code,
               counted by const_find_values(); in another function, of one
               of its own variables, never of a variable of the module */
            break;
        case OP_scope_put_var:
        case OP_scope_delete_var:
        case OP_scope_make_ref:
        case OP_scope_get_ref:
        case OP_scope_get_var_checkthis:
            candidate = const_find(search, get_u32(buf + pos + 1));
            if (candidate)
                candidate->is_rejected = TRUE;
            break;
        default:
            break;
        }
    }

    list_for_each(el, &fd->child_list) {
        const_check_uses(search, list_entry(el, JSFunctionDef, link));
    }
}

int js_find_module_constants(JSContext *ctx, JSFunctionDef *fd)
{
    ConstSearch search;
    JSModuleDef *m = fd->module;
    int i, k, kept, ret = -1;

    /* a module only, without direct eval (its variables are found by name
       at run time) */
    if (!m || fd->global_var_count == 0 || const_has_eval(fd))
        return 0;

    /* 1. the candidates */
    search.ctx = ctx;
    search.count = 0;
    search.candidates = js_mallocz(ctx, fd->global_var_count * sizeof(search.candidates[0]));
    if (!search.candidates)
        return -1;
    for(i = 0; i < fd->global_var_count; i++) {
        JSGlobalVar *hf = &fd->global_vars[i];
        if (hf->is_const && hf->is_lexical && hf->cpool_idx < 0) {
            search.candidates[search.count].name = hf->var_name;
            search.candidates[search.count].value = JS_UNINITIALIZED;
            search.count++;
        }
    }

    /* 2. their values, 3. their uses */
    if (search.count > 0) {
        if (const_find_values(&search, fd))
            goto done;
        const_check_uses(&search, fd);
    }

    /* 4. the constants of the module; an exported one keeps its variable */
    kept = 0;
    for(i = 0; i < search.count; i++) {
        ConstCandidate *candidate = &search.candidates[i];
        if (!JS_IsUninitialized(candidate->value) && !candidate->is_rejected && candidate->init_count == 1)
            kept++;
    }
    if (kept > 0) {
        fd->module_consts = js_mallocz(ctx, kept * sizeof(fd->module_consts[0]));
        if (!fd->module_consts)
            goto done;
        for(i = 0; i < search.count; i++) {
            ConstCandidate *candidate = &search.candidates[i];
            JSModuleConst *mc;
            if (JS_IsUninitialized(candidate->value) || candidate->is_rejected || candidate->init_count != 1)
                continue;
            mc = &fd->module_consts[fd->module_const_count++];
            mc->var_name = JS_DupAtom(ctx, candidate->name);
            mc->value = JS_DupValue(ctx, candidate->value);
            mc->is_removed = TRUE;
            for(k = 0; k < m->export_entries_count; k++) {
                JSExportEntry *me = &m->export_entries[k];
                if (me->export_type == JS_EXPORT_TYPE_LOCAL && me->local_name == candidate->name)
                    mc->is_removed = FALSE;
            }
        }
    }
    ret = 0;

 done:
    for(i = 0; i < search.count; i++)
        JS_FreeValue(ctx, search.candidates[i].value);
    js_free(ctx, search.candidates);
    return ret;
}

JSModuleConst *js_get_module_const(JSFunctionDef *fd, JSAtom name)
{
    int i;

    for(i = 0; i < fd->module_const_count; i++) {
        if (fd->module_consts[i].var_name == name)
            return &fd->module_consts[i];
    }
    return NULL;
}

int js_emit_module_const(JSContext *ctx, JSFunctionDef *fd, DynBuf *bc, JSModuleConst *mc)
{
    return fold_emit_value(ctx, fd, bc, mc->value);
}

void js_free_module_consts(JSContext *ctx, JSFunctionDef *fd)
{
    int i;

    for(i = 0; i < fd->module_const_count; i++) {
        JS_FreeAtom(ctx, fd->module_consts[i].var_name);
        JS_FreeValue(ctx, fd->module_consts[i].value);
    }
    js_free(ctx, fd->module_consts);
    fd->module_consts = NULL;
    fd->module_const_count = 0;
}
