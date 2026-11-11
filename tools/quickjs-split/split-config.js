/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Configuration of split.js for quickjs.c of QuickJS 2026-06-04.
// Line numbers refer to the unmodified upstream quickjs.c.
'use strict';

const version = '2026-06-04';

// The lines before this one become quickjs-internal.h.
const preludeEnd = 1416;

// [first line, file]: a range runs until the next entry.
const fileRanges = [
    [1416, 'core/runtime.c'],          // malloc, runtime, contexts
    [2868, 'core/atoms.c'],
    [3816, 'core/runtime.c'],          // classes
    [3923, 'core/strings.c'],
    [5119, 'core/objects.c'],          // shapes, object creation
    [6097, 'core/gc.c'],
    [7349, 'core/errors.c'],           // exceptions, backtraces, interrupts
    [7886, 'core/properties.c'],
    [11063, 'core/conversions.c'],
    [11278, 'core/bigint-arith.c'],
    [12763, 'core/conversions.c'],
    [13692, 'core/print.c'],
    [14556, 'core/operators.c'],
    [16110, 'core/iteration.c'],       // arguments, for-in, iterator protocol
    [16999, 'core/closures.c'],        // var refs, closures, C function calls
    [17772, 'core/interpreter.c'],
    [20776, 'core/generators.c'],
    [21668, 'compiler/tokenizer.c'],
    [23154, 'builtins/json.c'],        // JSON tokenizer
    [23530, 'compiler/tokenizer.c'],
    [23692, 'compiler/emitter.c'],
    [24358, 'compiler/parser-expr.c'],
    [24992, 'compiler/parser-class.c'],
    [25669, 'compiler/parser-expr.c'],
    [28201, 'compiler/parser-stmt.c'],
    [29547, 'core/modules.c'],
    [31477, 'compiler/parser-module.c'],
    [31945, 'compiler/funcdef.c'],
    [32577, 'compiler/resolve-vars.c'],
    [33764, 'compiler/resolve-labels.c'],
    [35595, 'compiler/finalize.c'],
    [36210, 'compiler/parser-func.c'],
    [37031, 'core/interpreter.c'],     // JS_EvalFunction, eval dispatch
    [37262, 'bytecode/writer.c'],
    [38243, 'bytecode/reader.c'],
    [39401, 'core/cfunc.c'],
    [39763, 'builtins/global.c'],
    [39790, 'builtins/object.c'],
    [40931, 'builtins/function.c'],    // Function and Error
    [41482, 'builtins/array.c'],
    [43469, 'builtins/iterator.c'],
    [44588, 'builtins/number.c'],      // Number, Boolean
    [44951, 'builtins/string.c'],
    [46698, 'builtins/math.c'],
    [47227, 'builtins/date.c'],
    [47327, 'builtins/regexp.c'],
    [49092, 'builtins/json.c'],
    [50185, 'builtins/proxy.c'],       // Reflect and Proxy
    [51341, 'builtins/symbol.c'],
    [51465, 'builtins/mapset.c'],
    [53119, 'builtins/promise.c'],
    [54493, 'builtins/global.c'],      // URI functions
    [54788, 'builtins/date.c'],
    [55947, 'compiler/eval.c'],        // JS_AddIntrinsicEval
    [55955, 'builtins/bigint.c'],
    [56152, 'core/intrinsics.c'],
    [56506, 'builtins/arraybuffer.c'],
    [57132, 'builtins/typedarray.c'],
    [58845, 'builtins/base64.c'],
    [59599, 'builtins/typedarray.c'],
    [59898, 'builtins/dataview.c'],
    [60288, 'builtins/atomics.c'],
    [60804, 'builtins/typedarray.c'],  // JS_AddIntrinsicTypedArrays
    [60908, 'builtins/weakref.c'],
];

// Individual items placed somewhere else than their line range.
const overrides = {
    free_function_bytecode: 'core/gc.c',
    free_bytecode_atoms: 'core/gc.c',
    JSOpCode: 'bytecode/opcodes.c',
    opcode_info: 'bytecode/opcodes.c',
    __JS_EvalInternal: 'compiler/eval.c',
    JS_ResolveModule: 'core/modules.c',
    ident_realloc: 'core/strings.c',
    get_line_col: 'core/errors.c',
    js_get_prototype_from_ctor: 'core/objects.c',
    add_export_entry: 'compiler/parser-module.c',
};

// ---------------------------------------------------------------------------
// Patches applied before the split.

const JSON_DECLARATIONS = `
/* JSON parser state, independent from the JavaScript parser */

enum JSONTokenType {
    JSON_TOK_NUMBER = -128,
    JSON_TOK_STRING,
    JSON_TOK_IDENT,
    JSON_TOK_ERROR,
    JSON_TOK_EOF,
};

typedef struct JSONToken {
    int val; /* a JSONTokenType or a single character */
    const uint8_t *ptr; /* position in the source */
    union {
        struct {
            JSValue str;
            int sep;
        } str;
        struct {
            JSValue val;
        } num;
        struct {
            JSAtom atom;
        } ident;
    } u;
} JSONToken;

typedef struct JSONParseState {
    JSContext *ctx;
    const char *filename;
    JSONToken token;
    const uint8_t *last_ptr;
    const uint8_t *buf_start;
    const uint8_t *buf_ptr;
    const uint8_t *buf_end;
    BOOL ext_json; /* true if accepting the JSON superset */
} JSONParseState;

static void json_parse_init(JSContext *ctx, JSONParseState *s,
                            const char *input, size_t input_len,
                            const char *filename)
{
    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    s->filename = filename;
    s->buf_start = s->buf_ptr = (const uint8_t *)input;
    s->buf_end = s->buf_ptr + input_len;
    s->token.val = ' ';
    s->token.ptr = s->buf_ptr;
}

static void json_free_token(JSONParseState *s)
{
    switch(s->token.val) {
    case JSON_TOK_NUMBER:
        JS_FreeValue(s->ctx, s->token.u.num.val);
        break;
    case JSON_TOK_STRING:
        JS_FreeValue(s->ctx, s->token.u.str.str);
        break;
    case JSON_TOK_IDENT:
        JS_FreeAtom(s->ctx, s->token.u.ident.atom);
        break;
    default:
        break;
    }
}

/* 'ptr' is the position of the error in the source */
static int json_parse_error_v(JSONParseState *s, const uint8_t *ptr,
                              const char *fmt, va_list ap)
{
    JSContext *ctx = s->ctx;
    int line_num, col_num;
    line_num = get_line_col(&col_num, s->buf_start, ptr - s->buf_start);
    JS_ThrowError2(ctx, JS_SYNTAX_ERROR, fmt, ap, FALSE);
    build_backtrace(ctx, ctx->rt->current_exception, s->filename,
                    line_num + 1, col_num + 1, 0);
    return -1;
}

static __attribute__((format(printf, 3, 4))) int json_parse_error_pos(JSONParseState *s, const uint8_t *ptr, const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = json_parse_error_v(s, ptr, fmt, ap);
    va_end(ap);
    return ret;
}

static __attribute__((format(printf, 2, 3))) int json_parse_error(JSONParseState *s, const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = json_parse_error_v(s, s->token.ptr, fmt, ap);
    va_end(ap);
    return ret;
}
`;

const JSON_RANGES = [[23154, 23529], [49092, 49467]];

const JSON_REPLACEMENTS = [
    [/\bJSParseState\b/g, 'JSONParseState'],
    [/\bjs_parse_error_pos\(/g, 'json_parse_error_pos('],
    [/\bjs_parse_error\(/g, 'json_parse_error('],
    [/\bfree_token\(s, &s->token\)/g, 'json_free_token(s)'],
    [/\bjs_parse_init\(/g, 'json_parse_init('],
    [/\bTOK_(STRING|NUMBER|IDENT|EOF|ERROR)\b/g, 'JSON_TOK_$1'],
    [/^[ \t]*s->token\.u\.ident\.(has_escape|is_reserved) = FALSE;\n/gm, ''],
];

const jsonPatch = {
    name: 'JSON parser with its own state',
    apply({ units, parseUnits, analyzeUnit }) {
        const inJsonRange = unit => JSON_RANGES.some(([a, b]) => unit.startLine >= a && unit.startLine <= b);
        for (const unit of units.filter(inJsonRange)) {
            for (const [pattern, replacement] of JSON_REPLACEMENTS) {
                unit.text = unit.text.replace(pattern, replacement);
            }
            analyzeUnit(unit);
        }
        const synthetic = parseUnits(JSON_DECLARATIONS, 0).units;
        synthetic.forEach((unit, k) => {
            unit.synthetic = true;
            unit.forcedFile = 'builtins/json.c';
            unit.startLine = 23153;
            unit.sortKey = 23153 + (k + 1) / 1000;
        });
        units.push(...synthetic);
    },
};

const ADD_EXPORT_ENTRY2 = `static JSExportEntry *add_export_entry2(JSContext *ctx, JSModuleDef *m,
                                       JSAtom local_name, JSAtom export_name,
                                       JSExportTypeEnum export_type)
{
    JSExportEntry *me;

    if (find_export_entry(ctx, m, export_name)) {
        JS_ThrowSyntaxErrorAtom(ctx, "duplicate exported name '%s'", export_name);
        return NULL;
    }

    if (js_resize_array(ctx, (void **)&m->export_entries,
                        sizeof(JSExportEntry),
                        &m->export_entries_size,
                        m->export_entries_count + 1))
        return NULL;
    me = &m->export_entries[m->export_entries_count++];
    memset(me, 0, sizeof(*me));
    me->local_name = JS_DupAtom(ctx, local_name);
    me->export_name = JS_DupAtom(ctx, export_name);
    me->export_type = export_type;
    return me;
}`;

const ADD_EXPORT_ENTRY = `/* same as add_export_entry2() but reports the error with the source position */
static JSExportEntry *add_export_entry(JSParseState *s, JSModuleDef *m,
                                       JSAtom local_name, JSAtom export_name,
                                       JSExportTypeEnum export_type)
{
    if (find_export_entry(s->ctx, m, export_name)) {
        char buf1[ATOM_GET_STR_BUF_SIZE];
        js_parse_error(s, "duplicate exported name '%s'",
                       JS_AtomGetStr(s->ctx, buf1, sizeof(buf1), export_name));
        return NULL;
    }
    return add_export_entry2(s->ctx, m, local_name, export_name, export_type);
}`;

const exportPatch = {
    name: 'module exports without parser state at run time',
    apply({ findUnit, analyzeUnit }) {
        const runtimeVersion = findUnit('add_export_entry2');
        runtimeVersion.text = ADD_EXPORT_ENTRY2;
        analyzeUnit(runtimeVersion);

        const parserVersion = findUnit('add_export_entry');
        parserVersion.text = ADD_EXPORT_ENTRY;
        analyzeUnit(parserVersion);

        const caller = findUnit('JS_AddModuleExport');
        const before = caller.text;
        caller.text = caller.text.replace('add_export_entry2(ctx, NULL, m,', 'add_export_entry2(ctx, m,');
        if (caller.text === before) {
            throw new Error('JS_AddModuleExport: call to add_export_entry2 not found');
        }
        analyzeUnit(caller);
    },
};

module.exports = {
    version,
    preludeEnd,
    fileRanges,
    overrides,
    patches: [jsonPatch, exportPatch],
};
