/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/*
 * QuickJS - compiler/tdz.c
 *
 * qjsx: removes the temporal dead zone checks the compiler can prove
 * useless. Not part of upstream QuickJS: see doc/engine-changes.md (N).
 *
 * resolve_variables() turns every read of a local let / const / class into
 * OP_get_loc_check and every write into OP_put_loc_check: they throw a
 * ReferenceError while the variable is uninitialized. Most of them run long
 * after the declaration, in code that cannot be reached before it. This
 * pass, between resolve_variables() and resolve_labels(), finds them:
 *
 *   1. a forward data-flow analysis, "the variable is surely initialized",
 *      one bit per local variable, over the bytecode of the function (labels
 *      still symbolic):
 *        - OP_enter_scope puts the variables of its scope in the dead zone
 *          (as resolve_labels() will emit OP_set_loc_uninitialized), except
 *          the function declarations it initializes;
 *        - OP_put_loc (the initialization of a declaration) and a
 *          successful OP_put_loc_check / OP_put_loc_check_init initialize;
 *        - at a label, a variable is initialized only if it is on every path
 *          that reaches it (intersection); the scan of the function is
 *          repeated until the states of the labels no longer change (loops);
 *        - a jump (if_false, if_true, goto, gosub, with_*) also reaches its
 *          label; goto, return, throw and ret end the path;
 *        - OP_catch: its handler can be reached from anywhere in the try
 *          block, where variables only become initialized (the scopes
 *          entered inside the try cannot be named by the handler): the state
 *          at OP_catch is the state of the handler;
 *        - OP_gosub (finally): the path goes on after it with its state (the
 *          finally block cannot uninitialize a variable visible there);
 *   2. each OP_get_loc_check / OP_put_loc_check on a variable surely
 *      initialized at that point becomes OP_get_loc / OP_put_loc, same size:
 *      nothing moves, and the optimizations of resolve_labels() (inc_loc,
 *      add_loc, set_loc, short opcodes) then apply as for 'var'.
 *
 * Left as they are: closure variables (another function may read them
 * before the initialization: OP_get_var_ref_check), 'this' of a derived
 * class constructor (initialized by super(), possibly from an arrow
 * function), functions calling eval() directly, and code the scan does not
 * reach. When unsure, the check stays: a check left is only slower, a check
 * removed wrongly would let a program read an uninitialized variable.
 */

#include "quickjs-internal.h"
#include "compiler/compiler.h"

/* past this many words of label states (bits = variables x labels), the pass
   is not run: the checks stay */
#define TDZ_MAX_STATE_WORDS (1 << 20)
/* past this many scans without reaching a fixed point, the checks stay */
#define TDZ_MAX_SCANS 1000

typedef struct TdzState {
    JSFunctionDef *fd;
    int words;            /* uint32_t per state: (var_count + 31) / 32 */
    uint32_t *labels;     /* label_count states: what reaches each label */
    uint8_t *visited;     /* label_count flags: a path reached the label */
    uint32_t *current;    /* the state at the current instruction */
    BOOL changed;         /* a label state changed during this scan */
} TdzState;

static inline BOOL tdz_is_set(const uint32_t *state, int idx)
{
    return (state[idx >> 5] >> (idx & 31)) & 1;
}

static inline void tdz_set(uint32_t *state, int idx)
{
    state[idx >> 5] |= 1u << (idx & 31);
}

static inline void tdz_clear(uint32_t *state, int idx)
{
    state[idx >> 5] &= ~(1u << (idx & 31));
}

/* The current path reaches 'label': its state becomes the intersection of
   what reached it so far and the current state. */
static void tdz_reach(TdzState *t, int label)
{
    uint32_t *target = t->labels + (size_t)label * t->words;
    int i;

    if (!t->visited[label]) {
        memcpy(target, t->current, t->words * sizeof(uint32_t));
        t->visited[label] = 1;
        t->changed = TRUE;
        return;
    }
    for(i = 0; i < t->words; i++) {
        uint32_t meet = target[i] & t->current[i];
        if (meet != target[i]) {
            target[i] = meet;
            t->changed = TRUE;
        }
    }
}

/* OP_enter_scope: the variables of 'scope' enter their dead zone, except
   the function declarations, which resolve_labels() initializes there. */
static void tdz_enter_scope(TdzState *t, int scope)
{
    JSFunctionDef *fd = t->fd;
    int idx;

    for(idx = fd->scopes[scope].first; idx >= 0;) {
        JSVarDef *vd = &fd->vars[idx];
        if (vd->scope_level != scope)
            break;
        if (vd->var_kind == JS_VAR_FUNCTION_DECL ||
            vd->var_kind == JS_VAR_NEW_FUNCTION_DECL)
            tdz_set(t->current, idx);
        else
            tdz_clear(t->current, idx);
        idx = vd->scope_next;
    }
}

/* One scan of the bytecode. When 'rewrite' is set, the checks on variables
   surely initialized are replaced; else only the label states change. */
static void tdz_scan(TdzState *t, BOOL rewrite)
{
    JSFunctionDef *fd = t->fd;
    uint8_t *bc = fd->byte_code.buf;
    int size = fd->byte_code.size;
    int pos, op, len, idx, label;
    BOOL reachable = TRUE;

    /* at the start of the function no variable is known to be initialized */
    memset(t->current, 0, t->words * sizeof(uint32_t));
    t->changed = FALSE;

    for(pos = 0; pos < size; pos += len) {
        op = bc[pos];
        len = opcode_info[op].size;

        /* a label: the paths that reach it meet; code after an end of path
           with no label reaching it is not analysed */
        if (op == OP_label) {
            label = get_u32(bc + pos + 1);
            if (reachable)
                tdz_reach(t, label);
            if (t->visited[label]) {
                memcpy(t->current, t->labels + (size_t)label * t->words,
                       t->words * sizeof(uint32_t));
                reachable = TRUE;
            } else {
                reachable = FALSE;
            }
            continue;
        }
        if (!reachable)
            continue;

        switch(op) {
        case OP_enter_scope:
            tdz_enter_scope(t, get_u16(bc + pos + 1));
            break;
        case OP_put_loc:
        case OP_put_loc_check_init:
            tdz_set(t->current, get_u16(bc + pos + 1));
            break;
        case OP_get_loc_check:
        case OP_put_loc_check:
            idx = get_u16(bc + pos + 1);
            if (rewrite && tdz_is_set(t->current, idx) &&
                idx != fd->this_var_idx) {
                bc[pos] = (op == OP_get_loc_check) ? OP_get_loc : OP_put_loc;
            }
            /* past the instruction (it threw otherwise), the variable is
               initialized */
            tdz_set(t->current, idx);
            break;
        case OP_if_false:
        case OP_if_true:
        case OP_catch:
        case OP_gosub:
            tdz_reach(t, get_u32(bc + pos + 1));
            break;
        case OP_goto:
            tdz_reach(t, get_u32(bc + pos + 1));
            reachable = FALSE;
            break;
        case OP_with_get_var:
        case OP_with_put_var:
        case OP_with_delete_var:
        case OP_with_make_ref:
        case OP_with_get_ref:
            tdz_reach(t, get_u32(bc + pos + 5));
            break;
        case OP_return:
        case OP_return_undef:
        case OP_return_async:
        case OP_throw:
        case OP_throw_error:
        case OP_ret:
            reachable = FALSE;
            break;
        default:
            break;
        }
    }
}

/* qjsx: called by js_create_function() between resolve_variables() and
   resolve_labels(). Returns -1 only when out of memory. */
int remove_useless_tdz_checks(JSContext *ctx, JSFunctionDef *fd)
{
    TdzState t;
    size_t label_words;
    int scan, ret = -1;

    /* nothing to do, or out of reach of the analysis: the checks stay */
    if (fd->var_count == 0 || fd->has_eval_call)
        return 0;
    t.fd = fd;
    t.words = (fd->var_count + 31) / 32;
    label_words = (size_t)fd->label_count * t.words;
    if (label_words > TDZ_MAX_STATE_WORDS)
        return 0;

    t.labels = js_mallocz(ctx, max_int(1, (int)label_words) * sizeof(uint32_t));
    t.visited = js_mallocz(ctx, max_int(1, fd->label_count));
    t.current = js_mallocz(ctx, t.words * sizeof(uint32_t));
    if (!t.labels || !t.visited || !t.current)
        goto done;

    /* 1. scan until the states of the labels no longer change */
    for(scan = 0; scan < TDZ_MAX_SCANS; scan++) {
        tdz_scan(&t, FALSE);
        if (!t.changed)
            break;
    }

    /* 2. a fixed point: a last scan replaces the useless checks (the states
       it reads are the final ones) */
    if (scan < TDZ_MAX_SCANS)
        tdz_scan(&t, TRUE);
    ret = 0;

 done:
    js_free(ctx, t.labels);
    js_free(ctx, t.visited);
    js_free(ctx, t.current);
    return ret;
}
