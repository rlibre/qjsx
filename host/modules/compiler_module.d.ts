/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * Compiles JavaScript source into QuickJS bytecode, as `qjsx build` does.
 *
 * The bytecode is only valid for an engine of the same
 * {@link bytecodeVersion}.
 *
 * @example
 * import { compile } from "sys/compiler";
 * const { bytecode, imports, warnings } = compile(source, "src/main.js", { strip: "all" });
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | ✓ ¹ | — | — | — |
 *
 * ¹ The qjsx tool (its commands and `qjsx run`), not the executables made by
 * `qjsx build --executable`. Devices have no compiler at all (no `eval` either).
 *
 * @module sys/compiler
 */
declare module "sys/compiler" {
    /**
     * Options of {@link compile}. Unknown fields are a TypeError.
     */
    export interface CompileOptions {
        /**
         * True (default): an ES module; false: a global script.
         */
        module?: boolean;

        /**
         * Debug info removed from the bytecode:
         * - `"none"`: keep everything (source text, positions, variable names);
         * - `"source"` (default): remove the source text only;
         * - `"all"`: remove all debug info. The module name stays, so
         *   `import.meta` and relative imports still work.
         */
        strip?: "none" | "source" | "all";

        /**
         * True: also return the debug info of the functions (`debug` of the
         * result, the "sidecar" of a package). Default false. The bytecode is
         * the same either way.
         */
        debug?: boolean;

        /**
         * False: `import()` is a SyntaxError at its position (programs and
         * libraries built by `qjsx build` / `qjsx mklib`). Default true.
         */
        dynamicImport?: boolean;
    }

    /**
     * One variable of a function (argument or closure variable). Internal
     * variables have names like `"<ret>"` or `"this"`.
     */
    export interface DebugVariable {
        /**
         * Its name; null for a nameless one.
         */
        name: string | null;

        /**
         * Bits: 1 lexical (`let`, `const`, `class`), 2 const, 4 captured (used
         * by an inner function), 8 a global variable (closures only).
         */
        flags: number;
    }

    /**
     * One local variable of a function.
     */
    export interface DebugLocal extends DebugVariable {
        /**
         * Index of its scope in {@link DebugFunction.scopes}.
         */
        scope: number;
    }

    /**
     * One scope (block) of a function.
     */
    export interface DebugScope {
        /**
         * Index of the enclosing scope; -1 for scope 0.
         */
        parent: number;

        /**
         * Start of its source range, a byte offset in the UTF-8 source.
         */
        start: number;

        /**
         * End of its source range (excluded), a byte offset.
         */
        end: number;
    }

    /**
     * Debug info of one compiled function (an element of `debug` in the
     * result of {@link compile}).
     */
    export interface DebugFunction {
        /**
         * The function's number in the module, also its index in `debug`.
         * Numbers follow the order in which the compiler finishes the
         * functions: inner functions before the one containing them, the
         * top-level code last.
         */
        number: number;

        /**
         * Its name; null for an anonymous function; `"<eval>"` for the
         * top-level code.
         */
        name: string | null;

        /**
         * Start of its source range, a byte offset in the UTF-8 source.
         */
        start: number;

        /**
         * End of its source range (excluded), a byte offset.
         */
        end: number;

        /**
         * Size of its bytecode, in bytes.
         */
        bytecodeLength: number;

        /**
         * Flat triples `[pc, sourceOffset, kind, pc, sourceOffset, kind, ...]`
         * sorted by pc (several may share a pc): bytecode offset, byte offset
         * in the source, and kind 0 expression (an instruction that can throw
         * or call), 1 statement start (where stepping and breakpoints stop:
         * statements, `catch` clauses, loop tests and updates), 2 return.
         */
        positions: number[];

        /**
         * Its arguments; the index is the argument slot.
         */
        arguments: DebugVariable[];

        /**
         * Its local variables; the index is the local slot.
         */
        locals: DebugLocal[];

        /**
         * The variables it captures from enclosing functions; the index is the
         * closure slot.
         */
        closures: DebugVariable[];

        /**
         * Its scopes, as source ranges.
         */
        scopes: DebugScope[];
    }

    /**
     * A construct that needs a compiler when the program runs, which a device
     * does not have (it throws there). Detection is syntactic: a direct
     * `eval(...)`, a call or `new` of `Function`; indirect uses are not seen.
     */
    export interface CompileWarning {
        /**
         * `"eval"` for a direct `eval(...)`, `"Function"` for `Function(...)` or
         * `new Function(...)`.
         */
        kind: "eval" | "Function";

        /**
         * Line, from 1.
         */
        line: number;

        /**
         * Column, from 1, as in a SyntaxError.
         */
        column: number;
    }

    /**
     * What {@link compile} returns.
     */
    export interface CompileResult {
        /**
         * The bytecode, in a new Uint8Array.
         */
        bytecode: Uint8Array;

        /**
         * The debug info of each function (index = function number) with
         * `debug: true`, else null.
         */
        debug: DebugFunction[] | null;

        /**
         * The modules the module asks for (`import`, `export ... from`), as
         * written in the source, in source order; a module may come several
         * times. Relative names are not resolved. `[]` for a script.
         */
        imports: string[];

        /**
         * The uses of `eval` and `Function` found, in source order.
         */
        warnings: CompileWarning[];
    }

    /**
     * Compiles `source` without running it; the imports of a module are not
     * loaded.
     *
     * Compiling a module registers it in the tool's context: compile each
     * module name once.
     *
     * @param source the JavaScript text.
     * @param name the module (or script) name recorded in the bytecode;
     * imports inside the module are resolved relatively to it, and errors name
     * it.
     * @throws SyntaxError (with `fileName` and `lineNumber`) for a syntax
     * error; TypeError for bad options.
     */
    export function compile(source: string, name: string, options: CompileOptions & { debug: true }): CompileResult & { debug: DebugFunction[] };
    /**
     * Compiles `source` without running it (see the overload with
     * `debug: true`).
     */
    export function compile(source: string, name: string, options?: CompileOptions): CompileResult;

    /**
     * The QuickJS version of the compiler, e.g. `"2026-06-04"`.
     */
    export const quickjsVersion: string;

    /**
     * The qjsx version of the tool, e.g. `"0.1.0"`.
     */
    export const qjsxVersion: string;

    /**
     * Version of the bytecode this compiler writes (an integer, 0x45 today):
     * an engine runs only bytecode of its own version.
     */
    export const bytecodeVersion: number;
}
