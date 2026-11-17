/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The native module "sys/draw" (draw_module.cpp; EVALUATION, 2026-11-17):
// registered by a port built with QJSX_DRAW.
#pragma once

#include "quickjs.h"

/**
 * Creates the module "sys/draw" (draw_module.cpp, JS_MODULE): the function
 * render.
 */
JSModuleDef *jsNewModule_draw(JSContext *ctx, const char *moduleName);
