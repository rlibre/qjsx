/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Fixture of tests/prune.test.js: what 'qjsx build' leaves out of the package
// (host/js/prune.js). The package must print what the sources print.
import { used, Shape, value as renamedValue } from "./lib.js";
import * as everything from "./everything.js";
import { reexported, starred } from "./reexport.js";
import { maybeEval } from "./evaluates.js";
import { Mutex } from "stdlib/tasks";

function unusedInMain() {
    return used(0);
}

// a function reached by a computed name: the object names it, it stays
const table = { pick: () => "picked" };
const key = "pick";

console.log(used(2), new Shape(3).area(), renamedValue);
console.log(everything.one(), everything["two"]());
console.log(reexported(), starred(), typeof maybeEval, typeof Mutex);
console.log(table[key]());
