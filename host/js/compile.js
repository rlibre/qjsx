/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Compilation of one module, shared by 'qjsx build' (packages) and
// 'qjsx mklib' (libraries): bytecode, debug sidecar, identifiers and the
// modules it imports.
//
// Identifiers:
//   buildId    = first 32 hex digits of sha256(bytecode)
//   sourceHash = sha256(source) in hex
//
// Sidecar: identities of the module, plus 'functions', the debug info given
// by the compiler for each function, index = function number (format in
// host/modules/compiler_module.cpp and quickjs-debug.h). Positions are byte
// offsets in the source; the debugger proxy converts them into lines and
// UTF-16 columns.
import { compile, quickjsVersion, qjsxVersion, bytecodeVersion } from "sys/compiler";
import { sha256 } from "sys/crypto";
import { hex, encodeText, placeOf } from "qjsx:util";
import { importsBytes } from "qjsx:bytes";

export const SIDECAR_FORMAT = 2;   // 2: with 'functions'

/**
 * Identifies the compiler: bytecode is only valid for the engine that made it.
 */
export function compilerId() {
    return `qjsx ${qjsxVersion} / QuickJS ${quickjsVersion}`;
}

// Version of the bytecode this compiler writes: an engine only reads the
// bytecode of its own version. Recorded in the manifest of packages and
// libraries; a device refuses a package that holds another one.
export const BYTECODE_VERSION = bytecodeVersion;

/**
 * Text of a compile warning ({ kind, line, column } given by the compiler):
 * what the module asks of the compiler when it runs, which a device, having
 * no compiler, cannot do.
 */
export function warningText(warning) {
    const construct = warning.kind === "eval" ? "eval()" : "the Function constructor";
    return `${construct} needs the compiler, which a device does not have: it throws there`;
}

/**
 * Name of the module that 'importer' means by 'specifier'. Same rule as the
 * engine's default normalizer (js_default_module_normalize_name in
 * engine/qjsx/core/modules.c), which the device applies when it loads the
 * module: a specifier that does not start with '.' is a name, taken as it is;
 * else its leading "./" and "../" are resolved against the folder of the
 * importer's name.
 */
export function normalizeImport(importer, specifier) {
    // a name: taken as it is
    if (!specifier.startsWith(".")) {
        return specifier;
    }

    // a relative specifier: start from the folder of the importer
    const lastSlash = importer.lastIndexOf("/");
    let folder = lastSlash < 0 ? "" : importer.slice(0, lastSlash);
    let rest = specifier;

    for (;;) {
        // "./" stays in the folder
        if (rest.startsWith("./")) {
            rest = rest.slice(2);
            continue;
        }

        // "../" removes the last element of the folder, unless there is none
        // left or it is itself "." or ".."
        const lastElement = folder.slice(folder.lastIndexOf("/") + 1);
        const canGoUp = rest.startsWith("../") && folder !== "" && lastElement !== "." && lastElement !== "..";
        if (!canGoUp) {
            break;
        }

        const cut = folder.lastIndexOf("/");
        folder = cut < 0 ? "" : folder.slice(0, cut);
        rest = rest.slice(3);
    }

    // what is left of the specifier, in what is left of the folder
    return folder === "" ? rest : `${folder}/${rest}`;
}

/**
 * Compiles the module 'name' (the name recorded in the bytecode, by which
 * the other modules import it) from its source text. The bytecode has no
 * debug info (no source text, positions nor variable names): it is what runs
 * on a device; the debug info goes into the sidecar. 'import()' is refused:
 * every module of a program must be known when it is built.
 * Returns { buildId, sourceHash, bytecode, sidecar, imports, warnings, outline }:
 *   - 'imports' has one { specifier, name, bytes, place } per module it
 *     imports, each module once: the text written in the source, the name of
 *     the module it means, true for an import of bytes ('with { type:
 *     "bytes" }', qjsx:bytes), and "file:line:column" of the text;
 *   - 'warnings' has one { kind, line, column } per use of eval() or of the
 *     Function constructor (see warningText);
 *   - 'outline': its top-level statements, imports and exports, which 'qjsx
 *     build' reads to leave out what nothing uses (qjsx:prune; format in
 *     host/modules/compiler_module.cpp).
 */
export function compileModule(name, source) {
    // compile: stripped bytecode, its debug info apart
    const options = { module: true, strip: "all", debug: true, dynamicImport: false, outline: true };
    const { bytecode, debug, imports, warnings, outline } = compile(source, name, options);

    // its identifiers and its sidecar
    const buildId = hex(sha256(bytecode)).slice(0, 32);
    const sourceHash = hex(sha256(source));
    const sidecar = {
        format: SIDECAR_FORMAT,
        path: name,
        buildId,
        sourceHash,
        functions: debug,
    };

    return {
        buildId,
        sourceHash,
        bytecode,
        sidecar,
        imports: importedModules(name, source, imports, outline),
        warnings,
        outline,
    };
}

/**
 * The modules a compiled module imports (see compileModule), each once,
 * with the first text that names it. 'imports' are the specifiers the
 * compiler gives, one per import statement; the requests of the outline,
 * in the same order, add their attributes and positions (none without
 * outline). Throws when an import has attributes other than { type:
 * "bytes" }, or when a module is imported both as bytes and as a module.
 */
function importedModules(name, source, imports, outline) {
    const sourceBytes = encodeText(source);
    const requests = outline?.requests;
    const imported = new Map();

    imports.forEach((specifier, index) => {
        // where it is written, and whether it asks for bytes
        const request = requests?.[index];
        const place = request !== undefined ? placeOf(name, sourceBytes, request.position) : name;
        const bytes = request !== undefined && importsBytes(request.attributes, place);
        const importedName = normalizeImport(name, specifier);

        // already imported: the same way
        const earlier = imported.get(importedName);
        if (earlier !== undefined) {
            if (earlier.bytes !== bytes) {
                throw new Error(`${place}: '${importedName}' is imported both as bytes and as a module`);
            }
            return;
        }

        imported.set(importedName, { specifier, name: importedName, bytes, place });
    });

    return [...imported.values()];
}
