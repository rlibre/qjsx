/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "testlib/greet" of the test library (tests/run-tests.sh): it imports
// a neighbour relatively and a module of the standard library, which the
// build of a program must both follow.
import { hello } from "./words";
import { join } from "stdlib/path";

export function greet(name) {
    return `${hello}, ${name}! (${join("lib", "greet")})`;
}
