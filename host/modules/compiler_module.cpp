/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "sys/compiler" (host only): compiles JS source into QuickJS bytecode.
//
//   import { compile } from "sys/compiler";
//   const { bytecode, debug, imports } = compile(source, "src/main.js",
//       { module: true, strip: "all", debug: true, dynamicImport: false });
//
// 'name' is the module name recorded in the bytecode: imports inside the
// module are resolved relatively to it. 'strip':
//   "none"    keep everything (source text, positions, variable names)
//   "source"  remove the source text only (default)
//   "all"     remove all debug info (source text, positions, variable
//             names). The module name stays (engine change, see
//             doc/engine-changes.md), so import.meta and import() work.
// 'debug': true to get the debug info of the functions (the "sidecar",
// see quickjs-debug.h), else 'debug' is null. The bytecode is the same
// either way. One record per function, index = function number:
//   { number, name, start, end, bytecodeLength,
//     positions: [pc, sourceOffset, kind, pc, sourceOffset, kind, ...]
//                (kind 0 expression, 1 statement, 2 return; sorted by pc),
//     arguments: [{ name, flags }], locals: [{ name, scope, flags }],
//     closures: [{ name, flags }], scopes: [{ parent, start, end }],
//     constants: [{ name, value }] }
// 'constants': with the function of a module only, the constants of the
// module that the compiler replaced by their value (a number or a string):
// no variable holds them, the debugger shows them from here.
// Source offsets are byte offsets in the UTF-8 source; flags are the
// JS_DEBUG_VARIABLE_* bits.
// 'dynamicImport': false makes 'import()' a syntax error, reported at its
// position (programs built by qjsx build: all their modules must be known
// when they are built). Default: true.
// 'imports' in the result: the modules the module asks for (import, export
// ... from), as written in the source, in source order; a module imported by
// several statements may come several times; [] for a script. The caller
// normalizes the relative ones against 'name' and follows them.
// 'warnings' in the result: what the source asks of the compiler at run
// time, which a device cannot do (it has no compiler). One record per place:
//   { kind: "eval" | "Function", line, column }   (from 1, as in a SyntaxError)
// "eval" is a direct eval(...), "Function" a call or a 'new' of Function;
// indirect uses are not seen (JS_SetCompileWarningHandler, quickjs-debug.h).
// 'outline': true gives 'outline' in the result (null otherwise, and for a
// script), what 'qjsx build' reads to leave out what nothing uses (engine
// change W, JS_SetModuleOutlineCollector):
//   { statements: [{ start, end, removable, declared: [names],
//                    references: [names] }],
//     imports: [{ module, imported ("*" for import * as), local, position }],
//     exports: [{ exported (null for export * from), local, module (null for
//                 a name of the module), start, end (a name of an export
//                 clause, else 0, 0) }],
//     hasEval }
// Positions are byte offsets in the UTF-8 source.
//
// Constants: 'quickjsVersion', 'qjsxVersion', and 'bytecodeVersion', the
// version of the bytecode this compiler writes: an engine only reads its own.
//
// Note: compiling a module registers it in the context of the tool; this is
// harmless for a command-line tool that compiles each file once.
#include "jsbind.h"
#include "memory.h"
#include "quickjs-debug.h"
#include "version.h"

namespace {

JS_ENUM(Strip, none, source, all);

// The options of compile() (JS_STRUCT: unknown fields are refused).
struct CompileOptions {
    bool  module = true;           // true: ES module, false: global script
    Strip strip = Strip::source;   // debug info removed from the bytecode
    bool  debug = false;           // true: also return the sidecar debug info
    bool  dynamicImport = true;    // false: 'import()' is a syntax error
    bool  outline = false;         // true: also return the outline of a module
};
JS_STRUCT(CompileOptions, module, strip, debug, dynamicImport, outline);

// ### Debug info ##############################################################
// The engine calls collectFunction() for each compiled function.

// Where collectFunction() puts the records during one compile() call.
struct DebugCollection {
    JSValue  functions;  // JS array of function records
    uint32_t count;      // number of records in 'functions'
};

/**
 * The string of an atom, or null for JS_ATOM_NULL (anonymous function).
 */
JSValue atomOrNull(JSContext *ctx, JSAtom atom) {
    if (atom == JS_ATOM_NULL) {
        return JS_NULL;
    }

    return JS_AtomToString(ctx, atom);
}

/**
 * object[name] = value, as a JS number.
 */
void setNumber(JSContext *ctx, JSValue object, const char *name, int64_t value) {
    JS_SetPropertyStr(ctx, object, name, JS_NewInt64(ctx, value));
}

/**
 * The positions of a function as one flat array of triples:
 * [pc, sourceOffset, kind, pc, sourceOffset, kind, ...].
 */
JSValue positionsToJs(JSContext *ctx, const JSDebugFunctionInfo *info) {
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;

    for (int i = 0; i < info->position_count; i++) {
        // pc, source offset, kind
        const JSDebugPosition &position = info->positions[i];
        JS_SetPropertyUint32(ctx, array, index++, JS_NewInt64(ctx, position.pc));
        JS_SetPropertyUint32(ctx, array, index++, JS_NewInt64(ctx, position.source_pos));
        JS_SetPropertyUint32(ctx, array, index++, JS_NewInt32(ctx, position.kind));
    }

    return array;
}

/**
 * Variables as an array of { name, flags }, the index being the slot
 * index. 'withScope': local variables also carry the index of their scope.
 */
JSValue variablesToJs(JSContext *ctx, const JSDebugVariable *variables, int count, bool withScope) {
    JSValue array = JS_NewArray(ctx);

    for (int i = 0; i < count; i++) {
        // { name, scope?, flags } at its slot index
        JSValue variable = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, variable, "name", atomOrNull(ctx, variables[i].name));
        if (withScope) {
            setNumber(ctx, variable, "scope", variables[i].scope);
        }

        setNumber(ctx, variable, "flags", variables[i].flags);
        JS_SetPropertyUint32(ctx, array, i, variable);
    }

    return array;
}

/**
 * The constants of a module replaced by their value, as an array of
 * { name, value }.
 */
JSValue constantsToJs(JSContext *ctx, const JSDebugFunctionInfo *info) {
    JSValue array = JS_NewArray(ctx);

    for (int i = 0; i < info->constant_count; i++) {
        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "name", JS_AtomToString(ctx, info->constants[i].name));
        JS_SetPropertyStr(ctx, entry, "value", JS_DupValue(ctx, info->constants[i].value));
        JS_SetPropertyUint32(ctx, array, i, entry);
    }

    return array;
}

/**
 * The scopes of a function as an array of { parent, start, end }: parent
 * is the index of the enclosing scope (-1 for scope 0), start / end the
 * source range [start, end).
 */
JSValue scopesToJs(JSContext *ctx, const JSDebugFunctionInfo *info) {
    JSValue array = JS_NewArray(ctx);

    for (int i = 0; i < info->scope_count; i++) {
        JSValue scope = JS_NewObject(ctx);
        setNumber(ctx, scope, "parent", info->scopes[i].parent);
        setNumber(ctx, scope, "start", info->scopes[i].source_start);
        setNumber(ctx, scope, "end", info->scopes[i].source_end);
        JS_SetPropertyUint32(ctx, array, i, scope);
    }

    return array;
}

/**
 * Debug info collector (JS_SetDebugInfoCollector): appends the record of one
 * function compiled by compile() with debug: true, before its debug info is stripped.
 */
void collectFunction(JSContext *ctx, const JSDebugFunctionInfo *info, void *opaque) {
    DebugCollection *collection = (DebugCollection *)opaque;

    // 'info' is only valid during the call: copy what it says into a record
    JSValue record = JS_NewObject(ctx);
    setNumber(ctx, record, "number", info->function_number);
    JS_SetPropertyStr(ctx, record, "name", atomOrNull(ctx, info->name));
    setNumber(ctx, record, "start", info->source_start);
    setNumber(ctx, record, "end", info->source_end);
    setNumber(ctx, record, "bytecodeLength", info->bytecode_length);

    // its positions, variables and scopes
    JS_SetPropertyStr(ctx, record, "positions", positionsToJs(ctx, info));
    JS_SetPropertyStr(ctx, record, "arguments", variablesToJs(ctx, info->arguments, info->argument_count, false));
    JS_SetPropertyStr(ctx, record, "locals", variablesToJs(ctx, info->locals, info->local_count, true));
    JS_SetPropertyStr(ctx, record, "closures", variablesToJs(ctx, info->closures, info->closure_count, false));
    JS_SetPropertyStr(ctx, record, "scopes", scopesToJs(ctx, info));

    // the constants of its module, with the module function only
    if (info->constant_count > 0) {
        JS_SetPropertyStr(ctx, record, "constants", constantsToJs(ctx, info));
    }

    // records come in function number order (numbers are given in
    // creation order), so the index is the number
    JS_SetPropertyUint32(ctx, collection->functions, collection->count++, record);
}

// Where collectWarning() puts the records during one compile() call.
struct WarningCollection {
    JSValue  warnings;  // JS array of warning records
    uint32_t count;     // number of records in 'warnings'
};

/**
 * Compile warning handler (JS_SetCompileWarningHandler): appends
 * { kind, line, column } for one construct that needs the compiler at run time.
 */
void collectWarning(JSContext *ctx, JSCompileWarningKind kind, int line, int column, void *opaque) {
    WarningCollection *collection = (WarningCollection *)opaque;
    const char *kindName = kind == JS_COMPILE_WARNING_EVAL ? "eval" : "Function";

    // { kind, line, column }, appended
    JSValue record = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, record, "kind", JS_NewString(ctx, kindName));
    setNumber(ctx, record, "line", line);
    setNumber(ctx, record, "column", column);
    JS_SetPropertyUint32(ctx, collection->warnings, collection->count++, record);
}

// ### Outline #################################################################
// The engine calls collectOutline() once the module is parsed (engine change W).

/**
 * The names as a JS array of strings.
 */
JSValue atomsToJs(JSContext *ctx, const JSAtom *atoms, int count) {
    JSValue array = JS_NewArray(ctx);

    for (int i = 0; i < count; i++) {
        JS_SetPropertyUint32(ctx, array, i, JS_AtomToString(ctx, atoms[i]));
    }

    return array;
}

/**
 * The statements as an array of { start, end, removable, declared,
 * references }.
 */
JSValue outlineStatementsToJs(JSContext *ctx, const JSModuleOutline *outline) {
    JSValue array = JS_NewArray(ctx);

    for (int i = 0; i < outline->statement_count; i++) {
        const JSOutlineStatement *statement = &outline->statements[i];
        JSValue record = JS_NewObject(ctx);
        setNumber(ctx, record, "start", statement->start);
        setNumber(ctx, record, "end", statement->end);
        JS_SetPropertyStr(ctx, record, "removable", JS_NewBool(ctx, statement->removable));
        JS_SetPropertyStr(ctx, record, "declared", atomsToJs(ctx, statement->declared, statement->declared_count));
        JS_SetPropertyStr(ctx, record, "references", atomsToJs(ctx, statement->references, statement->reference_count));
        JS_SetPropertyUint32(ctx, array, i, record);
    }

    return array;
}

/**
 * The imports as an array of { module, imported, local, position }.
 */
JSValue outlineImportsToJs(JSContext *ctx, const JSModuleOutline *outline) {
    JSValue array = JS_NewArray(ctx);

    for (int i = 0; i < outline->import_count; i++) {
        const JSOutlineImport *imported = &outline->imports[i];
        JSValue record = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, record, "module", JS_AtomToString(ctx, imported->module));
        JS_SetPropertyStr(ctx, record, "imported", JS_AtomToString(ctx, imported->imported));
        JS_SetPropertyStr(ctx, record, "local", JS_AtomToString(ctx, imported->local));
        setNumber(ctx, record, "position", imported->position);
        JS_SetPropertyUint32(ctx, array, i, record);
    }

    return array;
}

/**
 * The exports as an array of { exported, local, module, start, end }
 * ('exported' null for 'export * from', 'module' null for a name of the
 * module itself).
 */
JSValue outlineExportsToJs(JSContext *ctx, const JSModuleOutline *outline) {
    JSValue array = JS_NewArray(ctx);

    for (int i = 0; i < outline->export_count; i++) {
        const JSOutlineExport *exported = &outline->exports[i];
        JSValue record = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, record, "exported", atomOrNull(ctx, exported->exported));
        JS_SetPropertyStr(ctx, record, "local", JS_AtomToString(ctx, exported->local));
        JS_SetPropertyStr(ctx, record, "module", atomOrNull(ctx, exported->module));
        setNumber(ctx, record, "start", exported->start);
        setNumber(ctx, record, "end", exported->end);
        JS_SetPropertyUint32(ctx, array, i, record);
    }

    return array;
}

/**
 * Module outline collector (JS_SetModuleOutlineCollector): '*opaque' (a
 * JSValue) receives { statements, imports, exports, hasEval }.
 */
void collectOutline(JSContext *ctx, const JSModuleOutline *outline, void *opaque) {
    JSValue *result = (JSValue *)opaque;

    // { statements, imports, exports, hasEval }
    JSValue record = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, record, "statements", outlineStatementsToJs(ctx, outline));
    JS_SetPropertyStr(ctx, record, "imports", outlineImportsToJs(ctx, outline));
    JS_SetPropertyStr(ctx, record, "exports", outlineExportsToJs(ctx, outline));
    JS_SetPropertyStr(ctx, record, "hasEval", JS_NewBool(ctx, outline->has_eval));

    // it replaces what *result held
    JS_FreeValue(ctx, *result);
    *result = record;
}

// ### Imports #################################################################

/**
 * JS array of the names of the modules that the compiled module asks for, as
 * written in its source; an empty array for a script ('isModule' false).
 */
JSValue importsToJs(JSContext *ctx, JSValueConst compiled, bool isModule) {
    // a script asks for nothing
    JSValue imports = JS_NewArray(ctx);
    if (!isModule) {
        return imports;
    }

    // one string per requested module, in the order of the source
    JSModuleDef *module = (JSModuleDef *)JS_VALUE_GET_PTR(compiled);
    int count = JS_GetModuleRequestCount(module);

    for (int i = 0; i < count; i++) {
        JSAtom name = JS_GetModuleRequestName(ctx, module, i);
        JS_SetPropertyUint32(ctx, imports, (uint32_t)i, JS_AtomToString(ctx, name));
        JS_FreeAtom(ctx, name);
    }

    return imports;
}

// What compile() gathered besides the bytecode; the JS values are owned.
struct CompileExtras {
    JSValue debug;      // array of function records, or null without debug: true
    JSValue imports;    // array of module names
    JSValue warnings;   // array of warning records
    JSValue outline;    // outline of a module, or null without outline: true
};

/**
 * Frees the values of 'extras' (compile() failed).
 */
void freeExtras(JSContext *ctx, const CompileExtras *extras) {
    JS_FreeValue(ctx, extras->debug);
    JS_FreeValue(ctx, extras->imports);
    JS_FreeValue(ctx, extras->warnings);
    JS_FreeValue(ctx, extras->outline);
}

/**
 * { bytecode, debug, imports, warnings, outline } returned by compile();
 * takes ownership of the values of 'extras'.
 */
js::Value compileResult(JSContext *ctx, const js::Buffer *bytecode, const CompileExtras *extras) {
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "bytecode", bytecode->value().dup());
    JS_SetPropertyStr(ctx, result, "debug", extras->debug);
    JS_SetPropertyStr(ctx, result, "imports", extras->imports);
    JS_SetPropertyStr(ctx, result, "warnings", extras->warnings);
    JS_SetPropertyStr(ctx, result, "outline", extras->outline);
    return js::Value(ctx, result);
}

/**
 * The engine's strip flags (JS_SetStripInfo) for a 'strip' option.
 */
int stripFlags(Strip strip) {
    switch (strip) {
        case Strip::none: {
            return 0;
        }
        case Strip::source: {
            return JS_STRIP_SOURCE;
        }
        case Strip::all: {
            return JS_STRIP_DEBUG;
        }
    }

    return 0;
}

/**
 * JS compile(source, name, options): { bytecode, debug, imports, warnings,
 * outline }.
 * A syntax error is thrown to the caller.
 */
js::Value compile(js::Str source, js::Str name, js::Optional<CompileOptions> optionsArgument) {
    // the options, their defaults without them
    CompileOptions options = optionsArgument.has() ? optionsArgument.value() : CompileOptions();
    JSContext *ctx = js::context();
    JSRuntime *rt = JS_GetRuntime(ctx);

    // with debug: true, install collectFunction() for this call only
    DebugCollection collection = { JS_NULL, 0 };
    if (options.debug) {
        collection.functions = JS_NewArray(ctx);
        JS_SetDebugInfoCollector(rt, collectFunction, &collection);
    }

    // collect the compile warnings of this call
    WarningCollection warned = { JS_NewArray(ctx), 0 };
    JS_SetCompileWarningHandler(rt, collectWarning, &warned);

    // with outline: true, the outline of a module
    JSValue outline = JS_NULL;
    if (options.outline && options.module) {
        JS_SetModuleOutlineCollector(rt, collectOutline, &outline);
    }

    // set the runtime's strip flags for this call
    int previousStrip = JS_GetStripInfo(rt);
    JS_SetStripInfo(rt, stripFlags(options.strip));

    // compile without evaluating; a module is compiled alone: its imports are
    // not loaded (engine flag JS_EVAL_FLAG_NO_RESOLVE, doc/engine-changes.md).
    // JS strings converted by the binding are zero-terminated, as JS_Eval
    // requires. Whatever the result, restore the strip flags and remove the
    // collector and the warning handler; on a syntax error, drop the records
    // and leave the exception pending
    int type = options.module ? JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_NO_RESOLVE : JS_EVAL_TYPE_GLOBAL;
    int importFlag = options.dynamicImport ? 0 : JS_EVAL_FLAG_NO_DYNAMIC_IMPORT;
    int flags = type | importFlag | JS_EVAL_FLAG_COMPILE_ONLY;
    JSValue compiled = JS_Eval(ctx, source.data(), source.size(), name.data(), flags);
    JS_SetStripInfo(rt, previousStrip);
    JS_SetDebugInfoCollector(rt, nullptr, nullptr);
    JS_SetCompileWarningHandler(rt, nullptr, nullptr);
    JS_SetModuleOutlineCollector(rt, nullptr, nullptr);

    // what was gathered; a syntax error: dropped, the exception to the caller
    CompileExtras extras = { collection.functions, JS_UNDEFINED, warned.warnings, outline };
    if (JS_IsException(compiled)) {
        freeExtras(ctx, &extras);
        js::detail::markPendingException();   // the SyntaxError goes to the caller
        return js::Value();
    }

    // the modules it asks for, read while the compiled module is alive
    extras.imports = importsToJs(ctx, compiled, options.module);

    // write the compiled object as bytecode
    size_t size = 0;
    uint8_t *bytecode = JS_WriteObject(ctx, &size, compiled, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx, compiled);
    if (bytecode == nullptr) {
        freeExtras(ctx, &extras);
        js::detail::markPendingException();
        return js::Value();
    }

    // copy it into a Uint8Array and free the engine's copy
    js::Buffer copy = js::Buffer::copy(bytecode, size);
    js_free(ctx, bytecode);
    return compileResult(ctx, &copy, &extras);
}

// ### Link ####################################################################

/**
 * The bytes of a Uint8Array (not copied): nullptr when 'array' is not one.
 */
const uint8_t *bytesOf(JSContext *ctx, JSValueConst array, size_t *size) {
    // its buffer, then the part of it the array shows
    size_t offset = 0;
    size_t length = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, array, &offset, &length, nullptr);
    if (JS_IsException(buffer)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return nullptr;
    }

    size_t bufferSize = 0;
    const uint8_t *data = JS_GetArrayBuffer(ctx, &bufferSize, buffer);
    JS_FreeValue(ctx, buffer);
    if (data == nullptr) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return nullptr;
    }

    *size = length;
    return data + offset;
}

/**
 * JS link(modules): { names, modules }. 'modules': the bytecode of every
 * module of a package, as compile() gives it (each with the table of its own
 * names). Returns the same modules written with fixed name numbers
 * (JS_WRITE_OBJ_FIXED_ATOMS), in the same order, and 'names', the image of
 * the names of the whole package (JS_SetPackageAtoms): a device then runs
 * the bytecode where the package lies, and keeps no name in its RAM.
 *
 * How: a runtime of its own, whose only names are the engine's, reads every
 * module; the names they use get the numbers that follow the engine's, in
 * the order they come; these numbers are the ones written.
 */
js::Value link(js::Value modules) {
    JSContext *ctx = js::context();

    // how many modules
    JSValue lengthValue = JS_GetPropertyStr(ctx, modules.get(), "length");
    uint32_t count = 0;
    JS_ToUint32(ctx, &count, lengthValue);
    JS_FreeValue(ctx, lengthValue);

    // the runtime of the link: a context without built-in objects is enough
    // to read and write bytecode, and makes no name
    JSRuntime *linkRuntime = JS_NewRuntime();
    JSContext *linkContext = JS_NewContextRaw(linkRuntime);
    JSValue *read = (JSValue *)memory::allocate(sizeof(JSValue) * (count + 1));
    uint32_t readCount = 0;
    const char *problem = nullptr;

    // 1. read every module: its names take the next numbers
    for (uint32_t i = 0; i < count && problem == nullptr; i++) {
        // its bytes, read into the runtime of the link
        JSValue element = JS_GetPropertyUint32(ctx, modules.get(), i);
        size_t size = 0;
        const uint8_t *data = bytesOf(ctx, element, &size);
        if (data == nullptr) {
            problem = "compiler.link: every module must be a Uint8Array";
        } else {
            JSValue module = JS_ReadObject(linkContext, data, size, JS_READ_OBJ_BYTECODE);
            if (JS_IsException(module)) {
                JS_FreeValue(linkContext, JS_GetException(linkContext));
                problem = "compiler.link: a module is not bytecode of this compiler";
            } else {
                read[readCount++] = module;
            }
        }

        JS_FreeValue(ctx, element);
    }

    // 2. the image of the names, 3. each module written with their numbers
    JSValue result = JS_NewObject(ctx);
    JSValue written = JS_NewArray(ctx);
    if (problem == nullptr) {
        size_t namesSize = 0;
        uint8_t *names = JS_WritePackageAtoms(linkContext, &namesSize);
        if (names == nullptr) {
            JS_FreeValue(linkContext, JS_GetException(linkContext));
            problem = "compiler.link: the names of the package cannot be written";
        } else {
            JS_SetPropertyStr(ctx, result, "names", js::Buffer::copy(names, namesSize).value().dup());
            js_free(linkContext, names);
        }
    }

    for (uint32_t i = 0; i < readCount && problem == nullptr; i++) {
        // written with fixed name numbers, copied into a Uint8Array
        size_t size = 0;
        uint8_t *bytecode = JS_WriteObject(linkContext, &size, read[i], JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_FIXED_ATOMS);
        if (bytecode == nullptr) {
            JS_FreeValue(linkContext, JS_GetException(linkContext));
            problem = "compiler.link: a module cannot be written";
        } else {
            JS_SetPropertyUint32(ctx, written, i, js::Buffer::copy(bytecode, size).value().dup());
            js_free(linkContext, bytecode);
        }
    }

    // { names, modules }
    JS_SetPropertyStr(ctx, result, "modules", written);

    // free the modules, then the runtime of the link
    for (uint32_t i = 0; i < readCount; i++) {
        JS_FreeValue(linkContext, read[i]);
    }

    memory::release(read);
    JS_FreeContext(linkContext);
    JS_FreeRuntime(linkRuntime);

    // a problem: thrown, the result dropped
    if (problem != nullptr) {
        JS_FreeValue(ctx, result);
        return js::fail(problem);
    }

    return js::Value(ctx, result);
}

} // namespace

/**
 * Module "sys/compiler": compile(), link(), and the versions recorded in the
 * package manifest.
 */
JS_MODULE(compiler, module) {
    JS_FUNCTION(module, compile);
    JS_FUNCTION(module, link);

    // recorded in the package manifest: bytecode is only valid for the engine
    // version that produced it
    module.constant("quickjsVersion", CONFIG_VERSION);
    module.constant("qjsxVersion", QJSX_VERSION);
    module.constant("bytecodeVersion", JS_GetBytecodeVersion());

    // identifies the names of the engine: a package linked for them only runs
    // on an engine that has the same
    module.constant("engineNames", (double)JS_GetAtomsFingerprint());
}
