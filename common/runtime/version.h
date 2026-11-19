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
//
// NEVER CHANGED BY HAND (user, 2026-11-19), except to start a version that
// is not the next one (0.4.0 after 0.3.x). This number is the one of the NEXT
// release. tools/release.sh reads it, tags the commit "v<number>" and pushes
// the tag (GitHub then builds and publishes that release), and only then
// raises the last figure here and commits: the tag and this file never
// disagree. First figure after the zero: a release that may require a change
// in a program or a firmware to be flashed again; last figure: fixes only.
#pragma once

#define QJSX_VERSION "0.3.0"
