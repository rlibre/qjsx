/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Reads what "qjsx deploy --data" wrote to its data folder
// (tests/datafiles.test.js).
import { readFileSync } from "sys/fs";

console.log(`config: ${readFileSync("config.json", "utf8").trim()}`);
console.log(`note: ${readFileSync("sub/note.txt", "utf8").trim()}`);
