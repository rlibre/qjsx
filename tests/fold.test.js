/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Tests of the constant folding of the compiler (engine/qjsx/compiler/fold.c):
// an expression of literals gives what the same expression gives at run time
// (its operands hidden in an array), and its bytecode holds no operator.
// Run with: bin/Debug/qjsx run -A tests/fold.test.js
import { compile } from "sys/compiler";
import { check, equal, report } from "./assert.js";

// --- same value as at run time: the text of each literal expression
const CASES = [
    // numbers
    "5 * 1000", "1 << 4", "7 - 10", "1 + 2 + 3", "10 / 4", "8 / 2", "7 % 3", "-7 % 3", "-4 % 2", "2 ** 10",
    "2 ** 0.5", "1 / 0", "-1 / 0", "0 / 0", "0 * -1", "-0", "- 0 + 0", "1e308 * 10", "2147483647 + 1",
    "-2147483648 - 1", "65536 * 65536", "0.1 + 0.2", "1.5 * 2", "3 * 1.5 - 0.5",
    // shifts and bit operations
    "1 << 31", "1 << 32", "1 << 33", "-1 >>> 0", "-1 >>> 28", "-8 >> 1", "0xff & 0x0f", "0xf0 | 0x0f",
    "0xff ^ 0x0f", "~5", "~-1", "~4294967295", "+5", "- -5", "4294967296 | 0", "2147483648 | 0", "1.9 | 0",
    "-1.9 | 0", "1e20 >> 1", "5 & 1.9",
    // strings
    '"ab" + "cd"', '"qsdqsd" + "qsqsd"', '"v" + 1 + 2', '1 + 2 + "v"', '"a" + 1.5', '"a" + 1e21', '"a" + 0.000001',
    '"a" + 1e-7', '"a" + -0', '"a" + (1 / 0)', '"1" + "2"', '"1" + 2', '1 + "2"', '"" + ""', '"é€" + "ü"',
    '"a" + "\\u0000" + "b"', '"x" + 12 * 2', '"line\\n" + "tab\\t"',
    // left to run time, and still right
    '"😀" + "a"', '"\\ud83d" + "\\ude00"', "1n + 2n", "true + 1", "null + 1", '"a" + null', '"a" + undefined',
    '"a" + true', "1 < 2", '"a" < "b"', "typeof 1",
];

const LITERAL = /"(?:[^"\\]|\\.)*"|0x[0-9a-f]+|\d+n|\d+(?:\.\d+)?(?:e[-+]?\d+)?|true|null|undefined/gi;

/**
 * The value of 'expression' with every literal read from an array, so that
 * nothing is constant for the compiler.
 */
function atRunTime(expression) {
    // each literal becomes L[n]
    const literals = [];
    const text = expression.replace(LITERAL, literal => {
        literals.push(literal);
        return `L[${literals.length - 1}]`;
    });

    // the values of the literals, then the expression on them
    const values = literals.map(literal => new Function(`return ${literal};`)());
    return new Function("L", `return ${text};`)(values);
}

for (const expression of CASES) {
    const folded = new Function(`return ${expression};`)();
    const expected = atRunTime(expression);
    const shown = Object.is(expected, -0) ? "-0" : String(expected);
    check(Object.is(folded, expected) && typeof folded === typeof expected, `${expression} = ${shown}`);
}

// --- the bytecode: the result is there, the operands are not
function bytecodeOf(source) {
    return compile(source, "fold.js", { module: true, strip: "all" }).bytecode;
}

function contains(bytes, text) {
    const needle = new TextEncoder().encode(text);
    for (let i = 0; i + needle.length <= bytes.length; i++) {
        let k = 0;
        while (k < needle.length && bytes[i + k] === needle[k]) {
            k++;
        }
        if (k === needle.length) {
            return true;
        }
    }
    return false;
}

const joined = bytecodeOf('export const text = "qsdqsd" + "zzqsqsd";');
check(contains(joined, "qsdqsdzzqsqsd"), 'the bytecode of "qsdqsd" + "zzqsqsd" holds the joined string');
check(!contains(joined, "\u0007zzqsqsd"), "the right half is no longer a name of the bytecode");
const literal = bytecodeOf("export const delay = 5000;");
const computed = bytecodeOf("export const delay = 5 * 1000;");
equal(computed.length, literal.length, "5 * 1000 compiles to the bytecode of 5000");
const float = bytecodeOf("export const ratio = 0.75;");
const computedFloat = bytecodeOf("export const ratio = 3 / 4;");
equal(computedFloat.length, float.length, "3 / 4 compiles to the bytecode of 0.75");

// --- a conditional between constants is not folded across its branches
function pick(flag) {
    return (flag ? 1 : 2) + 3;
}
equal(`${pick(true)},${pick(false)}`, "4,5", "(flag ? 1 : 2) + 3");

function chain(value) {
    return 10 * (value || 2) + 1 * 2;
}
equal(`${chain(0)},${chain(5)}`, "22,52", "10 * (value || 2) + 1 * 2");

function many() {
    return 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12 + 13 + 14 + 15 + 16 + 17 + 18 + 19 + 20;
}
equal(many(), 210, "a long sum");

function list() {
    return [1 + 1, 2 * 2, "a" + "b", 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
        25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36 + 1, 38].join(",");
}
check(list().startsWith("2,4,ab,3,") && list().endsWith(",35,37,38"), "more constants in a row than the pass keeps aside");

// --- an error after a folded expression is still reported
function failing() {
    const size = 2 * 8;
    return undefined.length + size;
}

let message = "";
try {
    failing();
} catch (error) {
    message = error.message;
}
check(message.includes("undefined"), "an error after a folded expression");

report("fold");
