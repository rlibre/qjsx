// Compilation of one module, shared by 'espjs build' (packages) and
// 'espjs mklib' (libraries): bytecode, debug sidecar, identifiers and the
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
import { compile, quickjsVersion, espjsVersion, bytecodeVersion } from "compiler";
import { sha256 } from "crypto";
import { hex } from "espjs:util";

export const SIDECAR_FORMAT = 2;   // 2: with 'functions'

// Identifies the compiler: bytecode is only valid for the engine that made it.
export function compilerId() {
    return `espjs ${espjsVersion} / QuickJS ${quickjsVersion}`;
}

// Version of the bytecode this compiler writes: an engine only reads the
// bytecode of its own version. Recorded in the manifest of packages and
// libraries; a device refuses a package that holds another one.
export const BYTECODE_VERSION = bytecodeVersion;

// Text of a compile warning ({ kind, line, column } given by the compiler):
// what the module asks of the compiler when it runs, which a device, having
// no compiler, cannot do.
export function warningText(warning) {
    const construct = warning.kind === "eval" ? "eval()" : "the Function constructor";
    return `${construct} needs the compiler, which a device does not have: it throws there`;
}

// Name of the module that 'importer' means by 'specifier'. Same rule as the
// engine's default normalizer (js_default_module_normalize_name in
// engine/qjsx/core/modules.c), which the device applies when it loads the
// module: a specifier that does not start with '.' is a name, taken as it is;
// else its leading "./" and "../" are resolved against the folder of the
// importer's name.
export function normalizeImport(importer, specifier) {
    if (!specifier.startsWith(".")) {
        return specifier;
    }

    const lastSlash = importer.lastIndexOf("/");
    let folder = lastSlash < 0 ? "" : importer.slice(0, lastSlash);
    let rest = specifier;

    for (;;) {
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

    return folder === "" ? rest : `${folder}/${rest}`;
}

// Compiles the module 'name' (the name recorded in the bytecode, by which
// the other modules import it) from its source text. The bytecode has no
// debug info (no source text, positions nor variable names): it is what runs
// on a device; the debug info goes into the sidecar. 'import()' is refused:
// every module of a program must be known when it is built.
// Returns { buildId, sourceHash, bytecode, sidecar, imports, warnings }:
//   - 'imports' has one { specifier, name } per module it imports, each
//     module once: the text written in the source and the name of the module
//     it means;
//   - 'warnings' has one { kind, line, column } per use of eval() or of the
//     Function constructor (see warningText).
export function compileModule(name, source) {
    const options = { module: true, strip: "all", debug: true, dynamicImport: false };
    const { bytecode, debug, imports, warnings } = compile(source, name, options);

    const buildId = hex(sha256(bytecode)).slice(0, 32);
    const sourceHash = hex(sha256(source));
    const sidecar = {
        format: SIDECAR_FORMAT,
        path: name,
        buildId,
        sourceHash,
        functions: debug,
    };

    // the compiler gives one specifier per import statement: keep each
    // imported module once, with the first text that names it
    const imported = new Map();
    for (const specifier of imports) {
        const importedName = normalizeImport(name, specifier);
        if (!imported.has(importedName)) {
            imported.set(importedName, { specifier, name: importedName });
        }
    }

    return {
        buildId,
        sourceHash,
        bytecode,
        sidecar,
        imports: [...imported.values()],
        warnings,
    };
}
