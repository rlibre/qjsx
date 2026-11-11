/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Run by tests/run-tests.sh with qjsx (engine with compiler) and with
// qjsx-target (engine without compiler, like the ESP32 / RP2350 targets):
// the outputs must be the same, except the last line (eval).
import { Shape, Rectangle } from "./shapes.js";

const rectangle = new Rectangle(3, 4);
console.log("class", rectangle.name, rectangle.area(), Shape.count);

const [first, ...rest] = [1, 2, 3];
const { width, ...others } = { width: 1, height: 2, depth: 3 };
console.log("destructuring", first, rest.join("+"), width, JSON.stringify(others));

function* numbers(limit) {
    for (let i = 0; i < limit; i++) {
        yield i * i;
    }
}
console.log("generator", [...numbers(4)].join(","));

const map = new Map([["a", 1], ["b", 2]]);
const set = new Set([1, 2, 2, 3]);
console.log("collections", map.get("b"), set.size, [...map.keys()].join(""));

console.log("regexp", "2026-11-11".replace(/(\d+)-(\d+)-(\d+)/, "$3/$2/$1"));
console.log("json", JSON.stringify(JSON.parse('{"x":[1,{"y":null}]}')));
console.log("typed array", Array.from(new Uint8Array([250, 5]).map(x => x + 10)).join(","));
console.log("bigint", (2n ** 70n).toString());
console.log("template", `${rectangle.width}x${rectangle.height}`);
console.log("optional", rectangle.missing?.value ?? "none");

let failure;
try {
    null.property;
} catch (error) {
    failure = error.constructor.name;
}
console.log("exception", failure);

async function later(value) {
    await new Promise(resolve => setTimeout(resolve, 10));
    return value * 2;
}
console.log("async", await later(21));

try {
    console.log("eval", eval("1 + 1"));
} catch (error) {
    console.log("eval", `${error.name}: ${error.message}`);
}
