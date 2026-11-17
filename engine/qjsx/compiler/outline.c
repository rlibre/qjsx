/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/*
 * QuickJS - compiler/outline.c
 *
 * qjsx: outline of a module, given to the collector set by
 * JS_SetModuleOutlineCollector() (quickjs-debug.h): its top-level statements
 * (range, names declared, names used, whether it can be left out), its
 * imports and its exports. 'qjsx build' reads it to leave out of a package
 * the declarations that nothing uses (engine change W, doc/engine-changes.md).
 * Not part of upstream QuickJS.
 *
 * Flow, while the parser reads a module (js_parse_program):
 *   1. js_outline_statement_begin() before each top-level statement: notes
 *      where its bytecode starts, how many top-level variables, imports and
 *      exports the module has so far;
 *   2. js_outline_name_used() for each name the parser emits as a variable
 *      access (emit_atom after an OP_scope_* opcode), in nested functions too;
 *   3. js_outline_statement_end() after it: the names it declared are the
 *      top-level variables added since 1; it is removable when it declared
 *      some, imported nothing, and its bytecode only creates values;
 *   4. js_outline_report() once the module is parsed: the collector gets
 *      the statements, the imports and the exports of the module, and the
 *      modules it asks for with their import attributes (engine change Y:
 *      js_outline_request_position() notes where each one is named).
 */

#include "quickjs-internal.h"
#include "compiler/compiler.h"

/* A statement while it is built: its names are runs of the atom pool. */
typedef struct OutlineStatement {
    uint32_t start;              /* offset of its first token */
    uint32_t end;                /* offset after its last token */
    BOOL removable;              /* see JSOutlineStatement */
    int declared_first;          /* first of its declared names in 'atoms' */
    int declared_count;
    int references_first;        /* first of its used names in 'atoms' */
    int reference_count;
} OutlineStatement;

/* Range of a name of an export clause, by export entry. */
typedef struct OutlineRange {
    uint32_t start;
    uint32_t end;                /* 0: not a name of an export clause */
} OutlineRange;

typedef struct JSOutlineBuilder {
    OutlineStatement *statements;
    int statement_count;
    int statement_size;
    /* the names of every statement: its declared names, then its used names */
    JSAtom *atoms;
    int atom_count;
    int atom_size;
    /* offset of the imported name, by import entry of the module */
    uint32_t *import_positions;
    int import_position_size;
    /* the range of a name of an export clause, by export entry */
    OutlineRange *export_ranges;
    int export_range_size;
    /* offset of the string naming the module, by module request (engine
       change Y) */
    uint32_t *request_positions;
    int request_position_size;
    BOOL has_eval;
    /* the statement being parsed */
    BOOL in_statement;
    int bc_start;                /* size of the module's bytecode when it began */
    int global_var_start;        /* top-level variables of the module when it began */
    int closure_var_start;       /* imports of the module when it began */
} JSOutlineBuilder;

void JS_SetModuleOutlineCollector(JSRuntime *rt, JSModuleOutlineCollector *collector,
                                  void *opaque)
{
    rt->debug.outline_collector = collector;
    rt->debug.outline_opaque = opaque;
}

/* Starts the outline of the module 's' parses, if a collector asks for it.
   -1 when out of memory. */
int js_outline_start(JSParseState *s)
{
    JSOutlineBuilder *builder;

    if (!s->ctx->rt->debug.outline_collector)
        return 0;
    builder = js_mallocz(s->ctx, sizeof(*builder));
    if (!builder)
        return -1;
    s->outline = builder;
    return 0;
}

/* Appends a name to the pool. -1 when out of memory. */
static int outline_add_atom(JSParseState *s, JSAtom name)
{
    JSOutlineBuilder *b = s->outline;

    if (js_resize_array(s->ctx, (void **)&b->atoms, sizeof(b->atoms[0]),
                        &b->atom_size, b->atom_count + 1))
        return -1;
    b->atoms[b->atom_count++] = name;
    return 0;
}

/* 1. A top-level statement begins at the current token. */
void js_outline_statement_begin(JSParseState *s)
{
    JSOutlineBuilder *b = s->outline;
    JSFunctionDef *fd = s->cur_func;
    OutlineStatement *st;

    if (!b)
        return;
    if (js_resize_array(s->ctx, (void **)&b->statements, sizeof(b->statements[0]),
                        &b->statement_size, b->statement_count + 1))
        return;   /* out of memory: the statement is not recorded, the parse fails later */
    st = &b->statements[b->statement_count++];
    memset(st, 0, sizeof(*st));
    st->start = s->token.ptr - s->buf_start;
    st->declared_first = b->atom_count;
    st->references_first = b->atom_count;
    b->in_statement = TRUE;
    b->bc_start = fd->byte_code.size;
    b->global_var_start = fd->global_var_count;
    b->closure_var_start = fd->closure_var_count;
}

/* The name 'name' is declared by the statement (one of its top-level
   variables). */
static BOOL outline_is_declared(JSOutlineBuilder *b, const OutlineStatement *st, JSAtom name)
{
    int i;

    for(i = 0; i < st->declared_count; i++) {
        if (b->atoms[st->declared_first + i] == name)
            return TRUE;
    }
    return FALSE;
}

/* TRUE if the bytecode the statement emitted in the module function only
   creates values and stores them in its declared names: functions, classes
   without computed key nor static initializer (they emit a call), literals,
   objects and arrays of them, reads of variables. */
static BOOL outline_only_creates(JSOutlineBuilder *b, const OutlineStatement *st,
                                 JSFunctionDef *fd)
{
    const uint8_t *code = fd->byte_code.buf;
    int pos = b->bc_start;
    int end = fd->byte_code.size;

    while (pos < end) {
        int op = code[pos];
        switch(op) {
        /* markers */
        case OP_line_num:
        case OP_label:
        case OP_enter_scope:
        case OP_leave_scope:
        case OP_nop:
        /* values */
        case OP_push_i32:
        case OP_push_const:
        case OP_push_atom_value:
        case OP_push_bigint_i32:
        case OP_push_empty_string:
        case OP_undefined:
        case OP_null:
        case OP_push_false:
        case OP_push_true:
        case OP_fclosure:
        case OP_object:
        case OP_array_from:
        case OP_define_field:
        case OP_set_name:
        case OP_set_class_name:
        /* classes */
        case OP_define_class:
        case OP_define_method:
        case OP_set_home_object:
        case OP_private_symbol:
        case OP_add_brand:
        /* the stack */
        case OP_dup:
        case OP_drop:
        case OP_swap:
        case OP_nip:
        /* a read of a variable or of a property (the heritage of a class, an
           alias, a computed key such as [Symbol.iterator]): taken as without
           effect, a getter it would run is not run when it is left out */
        case OP_scope_get_var:
        case OP_get_field:
        case OP_to_propkey:
        case OP_define_method_computed:
        /* the initialization of a binding the statement creates (its
           declared names, the inner names of a class): never another one */
        case OP_scope_put_var_init:
            break;
        /* an assignment ('var' with an initializer): only to a name the
           statement declares */
        case OP_scope_put_var:
            if (!outline_is_declared(b, st, get_u32(code + pos + 1)))
                return FALSE;
            break;
        default:
            return FALSE;
        }
        pos += opcode_info[op].size;
    }
    return TRUE;
}

/* 3. The statement ends at the last token read. Its declared names are the
   top-level variables added since it began; it is removable when it
   declared some, imported nothing and only creates values. -1 when out of
   memory. */
int js_outline_statement_end(JSParseState *s)
{
    JSOutlineBuilder *b = s->outline;
    JSFunctionDef *fd = s->cur_func;
    OutlineStatement *st;
    int i, references_count, references_first;
    JSAtom *references;

    if (!b || !b->in_statement)
        return 0;
    b->in_statement = FALSE;
    st = &b->statements[b->statement_count - 1];
    st->end = s->last_ptr - s->buf_start;

    /* the used names were pooled while it was parsed: move them after its
       declared names (copy, then append the declared names, then the used
       ones again) */
    references_first = st->references_first;
    references_count = b->atom_count - references_first;
    references = NULL;
    if (references_count > 0) {
        references = js_malloc(s->ctx, references_count * sizeof(JSAtom));
        if (!references)
            return -1;
        memcpy(references, b->atoms + references_first, references_count * sizeof(JSAtom));
    }
    b->atom_count = references_first;

    st->declared_first = b->atom_count;
    for(i = b->global_var_start; i < fd->global_var_count; i++) {
        if (outline_add_atom(s, fd->global_vars[i].var_name))
            goto fail;
        st->declared_count++;
    }
    st->references_first = b->atom_count;
    for(i = 0; i < references_count; i++) {
        if (outline_add_atom(s, references[i]))
            goto fail;
        st->reference_count++;
    }
    js_free(s->ctx, references);

    st->removable = st->declared_count > 0 &&
        fd->closure_var_count == b->closure_var_start &&
        outline_only_creates(b, st, fd);
    return 0;
 fail:
    js_free(s->ctx, references);
    return -1;
}

/* 2. The parser emits an access to the variable 'name' (emit_atom after an
   OP_scope_* opcode): a use by the current statement, recorded once. The
   initialization of a declared name is not a use. */
void js_outline_name_used(JSParseState *s, JSAtom name)
{
    JSOutlineBuilder *b = s->outline;
    JSFunctionDef *fd = s->cur_func;
    OutlineStatement *st;
    int op, i;

    if (!b || !b->in_statement)
        return;

    /* only the name operand of a variable access */
    if (fd->last_opcode_pos + 1 != fd->byte_code.size)
        return;
    op = fd->byte_code.buf[fd->last_opcode_pos];
    switch(op) {
    case OP_scope_get_var_undef:
    case OP_scope_get_var:
    case OP_scope_put_var:
    case OP_scope_delete_var:
    case OP_scope_make_ref:
    case OP_scope_get_ref:
    case OP_scope_get_var_checkthis:
        break;
    default:
        return;
    }

    /* once per statement */
    st = &b->statements[b->statement_count - 1];
    for(i = st->references_first; i < b->atom_count; i++) {
        if (b->atoms[i] == name)
            return;
    }
    outline_add_atom(s, name);   /* out of memory: the name is not recorded, the parse fails later */
}

/* Makes the array '*array' ('*size' elements of 'element_size' bytes
   allocated) hold the element 'index', the elements added zeroed. -1 when
   out of memory. */
static int outline_reserve(JSParseState *s, void **array, int element_size, int *size, int index)
{
    int size_before = *size;

    if (js_resize_array(s->ctx, array, element_size, size, index + 1))
        return -1;
    if (*size > size_before) {
        memset((uint8_t *)*array + size_before * element_size, 0,
               (*size - size_before) * element_size);
    }
    return 0;
}

/* The imported name of import entry 'import_index' is at 'ptr'. -1 when
   out of memory. */
int js_outline_import_position(JSParseState *s, int import_index, const uint8_t *ptr)
{
    JSOutlineBuilder *b = s->outline;

    if (!b)
        return 0;
    if (outline_reserve(s, (void **)&b->import_positions, sizeof(b->import_positions[0]),
                        &b->import_position_size, import_index))
        return -1;
    b->import_positions[import_index] = ptr - s->buf_start;
    return 0;
}

/* The string naming the module of request 'request_index' is at 'ptr'
   (engine change Y). -1 when out of memory. */
int js_outline_request_position(JSParseState *s, int request_index, const uint8_t *ptr)
{
    JSOutlineBuilder *b = s->outline;

    if (!b)
        return 0;
    if (outline_reserve(s, (void **)&b->request_positions, sizeof(b->request_positions[0]),
                        &b->request_position_size, request_index))
        return -1;
    b->request_positions[request_index] = ptr - s->buf_start;
    return 0;
}

/* Export entry 'export_index' is a name of an export clause, written at
   [start, end) with the comma that follows it. -1 when out of memory. */
int js_outline_export_range(JSParseState *s, int export_index, const uint8_t *start,
                            const uint8_t *end)
{
    JSOutlineBuilder *b = s->outline;

    if (!b)
        return 0;
    if (outline_reserve(s, (void **)&b->export_ranges, sizeof(b->export_ranges[0]),
                        &b->export_range_size, export_index))
        return -1;
    b->export_ranges[export_index].start = start - s->buf_start;
    b->export_ranges[export_index].end = end - s->buf_start;
    return 0;
}

/* A direct eval(...) in the module: any of its names may be used. */
void js_outline_eval_seen(JSParseState *s)
{
    if (s->outline)
        s->outline->has_eval = TRUE;
}

/* 4. The module is parsed: gives its outline to the collector. Nothing is
   given when out of memory (the build then keeps every declaration). */
void js_outline_report(JSParseState *s, JSModuleDef *m)
{
    JSContext *ctx = s->ctx;
    JSOutlineBuilder *b = s->outline;
    JSFunctionDef *fd = s->cur_func;
    JSOutlineStatement *statements = NULL;
    JSOutlineImport *imports = NULL;
    JSOutlineExport *exports = NULL;
    JSOutlineRequest *requests = NULL;
    JSModuleOutline outline;
    int i, export_count;

    if (!b)
        return;

    /* the statements, their names taken from the pool */
    statements = js_mallocz(ctx, sizeof(statements[0]) * max_int(b->statement_count, 1));
    if (!statements)
        goto done;
    for(i = 0; i < b->statement_count; i++) {
        const OutlineStatement *st = &b->statements[i];
        statements[i].start = st->start;
        statements[i].end = st->end;
        statements[i].removable = st->removable;
        statements[i].declared = b->atoms + st->declared_first;
        statements[i].declared_count = st->declared_count;
        statements[i].references = b->atoms + st->references_first;
        statements[i].reference_count = st->reference_count;
    }

    /* the imports: the local name is the module's closure variable */
    imports = js_mallocz(ctx, sizeof(imports[0]) * max_int(m->import_entries_count, 1));
    if (!imports)
        goto done;
    for(i = 0; i < m->import_entries_count; i++) {
        const JSImportEntry *mi = &m->import_entries[i];
        imports[i].module = m->req_module_entries[mi->req_module_idx].module_name;
        imports[i].imported = mi->is_star ? JS_ATOM__star_ : mi->import_name;
        imports[i].local = fd->closure_var[mi->var_idx].var_name;
        imports[i].position = i < b->import_position_size ? b->import_positions[i] : 0;
    }

    /* the exports, then the 'export * from' */
    export_count = m->export_entries_count + m->star_export_entries_count;
    exports = js_mallocz(ctx, sizeof(exports[0]) * max_int(export_count, 1));
    if (!exports)
        goto done;
    for(i = 0; i < m->export_entries_count; i++) {
        const JSExportEntry *me = &m->export_entries[i];
        exports[i].exported = me->export_name;
        exports[i].local = me->local_name;
        exports[i].module = me->export_type == JS_EXPORT_TYPE_INDIRECT ?
            m->req_module_entries[me->u.req_module_idx].module_name : JS_ATOM_NULL;
        if (i < b->export_range_size) {
            exports[i].start = b->export_ranges[i].start;
            exports[i].end = b->export_ranges[i].end;
        }
    }
    for(i = 0; i < m->star_export_entries_count; i++) {
        JSOutlineExport *e = &exports[m->export_entries_count + i];
        e->exported = JS_ATOM_NULL;
        e->local = JS_ATOM__star_;
        e->module = m->req_module_entries[m->star_export_entries[i].req_module_idx].module_name;
    }

    /* the modules asked for, with their attributes (engine change Y) */
    requests = js_mallocz(ctx, sizeof(requests[0]) * max_int(m->req_module_entries_count, 1));
    if (!requests)
        goto done;
    for(i = 0; i < m->req_module_entries_count; i++) {
        const JSReqModuleEntry *rme = &m->req_module_entries[i];
        requests[i].module = rme->module_name;
        requests[i].attributes = rme->attributes;
        requests[i].position = i < b->request_position_size ? b->request_positions[i] : 0;
    }

    outline.statements = statements;
    outline.statement_count = b->statement_count;
    outline.imports = imports;
    outline.import_count = m->import_entries_count;
    outline.exports = exports;
    outline.export_count = export_count;
    outline.requests = requests;
    outline.request_count = m->req_module_entries_count;
    outline.has_eval = b->has_eval;
    ctx->rt->debug.outline_collector(ctx, &outline, ctx->rt->debug.outline_opaque);
 done:
    js_free(ctx, statements);
    js_free(ctx, imports);
    js_free(ctx, exports);
    js_free(ctx, requests);
}

/* Frees the outline of 's' (after the parse, whatever its result). */
void js_outline_free(JSParseState *s)
{
    JSOutlineBuilder *b = s->outline;

    if (!b)
        return;
    js_free(s->ctx, b->statements);
    js_free(s->ctx, b->atoms);
    js_free(s->ctx, b->import_positions);
    js_free(s->ctx, b->export_ranges);
    js_free(s->ctx, b->request_positions);
    js_free(s->ctx, b);
    s->outline = NULL;
}
