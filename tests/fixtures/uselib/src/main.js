/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Program that uses two libraries (tests/run-tests.sh): its package is run
// by the engine without compiler, like on a device, and must print
// expected.txt.
import { greet } from "testlib/greet";
import { basename } from "stdlib/path";
import { sha256 } from "crypto";

console.log(greet("device"));
console.log(basename("/a/b/c.txt"));
console.log("sha256 length:", sha256("x").length);
