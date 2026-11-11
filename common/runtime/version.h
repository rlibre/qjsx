/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Version of qjsx, the same for the tool and the firmwares (the QuickJS
// version is CONFIG_VERSION). The device agent sends it in its answer to
// HELLO; the ESP-IDF build reads it from this file for PROJECT_VER
// (ports/esp32/CMakeLists.txt): keep the #define on one line.
#pragma once

#define QJSX_VERSION "0.1.0"
