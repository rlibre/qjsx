/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Imports a native module by its old bare name: qjsx build refuses it (native
// modules are named sys/... or device/...; tests/run-tests.sh).
import { readFileSync } from "fs";

console.log(readFileSync("x"));
