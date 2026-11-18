/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The reader of bytecode against a damaged input (engine change Z,
// doc/engine-changes.md): a function whose byte_code ends in a truncated
// opcode is refused before its operand is read (it read and rewrote up to 4
// bytes past the buffer). Run by qjsx-tests (module jsbind_test).
//
//   qjsx-tests run tests/bytecode.test.js
import { readBytecode } from "jsbind_test";
import { bytecodeVersion } from "sys/compiler";
import { check, equal, report } from "./assert.js";

// What the reader expects (quickjs-internal.h, quickjs-opcode.h).
const BC_TAG_FUNCTION_BYTECODE = 12;
const OP_PUSH_ATOM_VALUE = 4;          // 5 bytes: the opcode, an atom (u32)

// A function without arguments, variables, closures or constants whose
// byte_code is 'code': the version, no atom of its own, the tag, its flags
// (u16) and mode, then its 12 counts and names (leb128, all 0 but the length
// of the code), then the code.
function functionBytes(code) {
    const header = [
        bytecodeVersion, 0, BC_TAG_FUNCTION_BYTECODE,
        0, 0,                       // flags
        0,                          // js_mode
        0, 0,                       // func_name, filename: no name
        0, 0, 0, 0, 0, 0,           // function_number, arg_count, var_count, defined_arg_count, stack_size, var_ref_count
        0, 0,                       // closure_var_count, cpool_count
        code.length,                // byte_code_len
        0,                          // local_count
    ];
    return new Uint8Array([...header, ...code]);
}

// a whole OP_push_atom_value (its atom the tagged integer 0) is read
equal(readBytecode(functionBytes([OP_PUSH_ATOM_VALUE, 0, 0, 0, 0x80])), "ok", "a whole opcode with an atom is read");

// the same cut after its opcode byte, then after 3 bytes of its operand
const cut = readBytecode(functionBytes([OP_PUSH_ATOM_VALUE]));
check(cut.includes("truncated opcode"), `an opcode without its operand is refused (${cut})`);

const shortOperand = readBytecode(functionBytes([OP_PUSH_ATOM_VALUE, 0, 0, 0]));
check(shortOperand.includes("truncated opcode"), `an opcode with 3 of its 4 operand bytes is refused (${shortOperand})`);

report("bytecode");
