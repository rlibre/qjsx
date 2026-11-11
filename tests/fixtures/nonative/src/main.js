/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Imports a native module that the desktop agent does not have: tests/run-tests.sh checks
// that qjsx deploy refuses it before sending anything.
import { Gpio } from "device/gpio";

new Gpio(1, { mode: "output" }).write(1);
