/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Demo project: the same output is expected when run from the sources and
// from the package (bytecode).
import { greet } from "./lib/greet.js";
import { add } from "./lib/math/add.js";
import { join } from "stdlib/path";
import { sha256 } from "crypto";

console.log(greet("qjsx"));
console.log("main module:", import.meta.main);
console.log("2 + 3 =", add(2, 3));
console.log("sha256 prefix:", Array.from(sha256("x").subarray(0, 4)).join(","));

await new Promise(resolve => setTimeout(resolve, 5));
console.log("after await:", join("a", "b"));

class Counter {
    #count = 0;
    increment() {
        return ++this.#count;
    }
}
const counter = new Counter();
counter.increment();
console.log("counter:", counter.increment());
