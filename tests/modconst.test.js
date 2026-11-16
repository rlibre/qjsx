/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tests of the constants of a module replaced by their value by the compiler
// (engine/qjsx/compiler/fold.c, js_find_module_constants): this file is such
// a module, and so is its fixture.
// Run with: bin/Debug/qjsx run -A tests/modconst.test.js
import { compile } from "sys/compiler";
import { LIMIT, NAME, TOTAL, steps, bump, counter } from "./fixtures/modconst/values.js";
import * as values from "./fixtures/modconst/values.js";
import { check, equal, report } from "./assert.js";

// --- read before the declaration below runs: the value, no ReferenceError
// (agreed change: QuickJS throws there)
equal(early(), 12, "a constant read before its declaration gives its value");

function early() {
    return TWELVE;
}

// --- the constants of this module
const TWELVE = 12;
const DELAY = 5 * 1000;
const MASK = 1 << 4;
const LABEL = "pin" + "-" + MASK;
const RATIO = 3 / 4;
const DIGITS = "12";
const NEGATIVE = -TWELVE;
const SUM = DELAY + MASK + TWELVE;
const TABLE = [1, 2, 3];              // not a literal: stays a variable
const COMPUTED = Math.max(1, 2);      // not a literal: stays a variable
let changing = 1;                     // not a const: stays a variable
const shadowed = 7;

equal([TWELVE, DELAY, MASK, LABEL, RATIO, DIGITS, NEGATIVE, SUM].join(), "12,5000,16,pin-16,0.75,12,-12,5028",
    "constants at the top level");
equal(typeof DIGITS, "string", "a string of digits stays a string");
equal(typeof TWELVE, "number", "typeof a constant");
equal(TABLE.length + COMPUTED + changing, 6, "what is not a literal constant still works");

// --- in functions, closures, classes, default values, object literals
function inFunction(extra = TWELVE) {
    const inner = () => DELAY + extra;
    return inner();
}
equal(inFunction(), 5012, "a constant in a closure and in a default value");
equal(inFunction(1), 5001, "the default value is not used when an argument is given");

class Reader {
    static size = MASK;
    field = LABEL;

    read() {
        return { TWELVE, mask: MASK, label: this.field };
    }
}
equal(JSON.stringify(new Reader().read()), '{"TWELVE":12,"mask":16,"label":"pin-16"}', "a constant in a class");
equal(Reader.size, 16, "a constant in a static field");

// --- a local variable of the same name wins
function local() {
    const TWELVE = "local";
    return TWELVE;
}
equal(local(), "local", "a local variable hides the constant");

function parameter(DELAY) {
    return DELAY;
}
equal(parameter("argument"), "argument", "a parameter hides the constant");

// a function assigns to a local variable named as a constant of the module:
// that constant keeps its variable (the names are not resolved when the
// constants are chosen), and everything still works
function assigns() {
    let shadowed = 1;
    shadowed += 1;
    return shadowed;
}
equal(`${assigns()},${shadowed}`, "2,7", "a constant whose name is assigned to elsewhere");

// --- writing to a constant still throws (it keeps its variable)
const WRITTEN = 1;

function write() {
    WRITTEN = 2;
}

let writeError = "";
try {
    write();
} catch (error) {
    writeError = error.constructor.name;
}
equal(`${writeError},${WRITTEN}`, "TypeError,1", "an assignment to a constant throws");

// --- the constants of the fixture: exported ones are imported as before
equal(`${LIMIT},${NAME},${TOTAL}`, "40,board,48", "exported constants are imported");
equal(steps().join(), "4,8,48,board", "constants read inside their module");
equal(`${values.LIMIT},${values.TOTAL},${Object.keys(values).sort().join()}`, "40,48,LIMIT,NAME,TOTAL,bump,counter,steps",
    "the namespace of the module holds its exported constants only");
equal(`${bump()},${counter}`, "4,4", "a variable of the module that changes is still a variable");

// --- the bytecode: a constant not exported costs nothing
function bytecodeOf(source) {
    return compile(source, "modconst.js", { module: true, strip: "all" }).bytecode;
}

const withConstant = bytecodeOf("const A = 5; export function f() { return A; }");
const withLiteral = bytecodeOf("export function f() { return 5; }");
equal(withConstant.length, withLiteral.length, "a constant read in a function compiles as its literal");

const chained = bytecodeOf('const A = 5; const B = A * 2; const C = "v" + B; export function f() { return C; }');
const chainedLiteral = bytecodeOf('export function f() { return "v10"; }');
equal(chained.length, chainedLiteral.length, "constants made of constants");

const exported = bytecodeOf("export const A = 5; export function f() { return A; }");
check(exported.length > withConstant.length, "an exported constant keeps its variable");

const reassigned = bytecodeOf("const A = 5; export function f() { try { A = 6; } catch (error) { return String(error); } return A; }");
check(reassigned.length > withConstant.length, "a constant that is assigned to keeps its variable");

report("modconst");
