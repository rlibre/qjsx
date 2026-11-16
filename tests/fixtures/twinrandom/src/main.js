/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Prints one line of random values (tests/random.test.js): the same at each
// run on a twin with "randomSeed", different on a twin without one.
import { randomBytes } from "sys/crypto";

const values = crypto.getRandomValues(new Uint8Array(8)).toHex();
const uuid = crypto.randomUUID();
const bytes = randomBytes(4).toHex();
console.log(`random: ${values} ${uuid} ${bytes} ${Math.random()}`);
