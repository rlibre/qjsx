/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The hardware modules of the boards (device/...), written once on top of the
// hal interfaces (common/hal/). A port compiles those whose hal it implements
// (ports/cmake/qjsx.cmake: QJSX_DEVICE_SOURCES) and registers them in its
// entry point, before agent::runDevice():
//
//   rt::registerNativeModule("device/gpio", &jsNewModule_gpio);
//
// The desktop compiles none of them.
#pragma once

#include "quickjs.h"

/**
 * Creates the module "device/gpio" (gpio_module.cpp, JS_MODULE): the class
 * Gpio. Needs hal_gpio.h.
 */
JSModuleDef *jsNewModule_gpio(JSContext *ctx, const char *moduleName);

/**
 * Creates the module "device/rmt" (rmt_module.cpp, JS_MODULE): the class
 * Rmt. Needs hal_rmt.h.
 */
JSModuleDef *jsNewModule_rmt(JSContext *ctx, const char *moduleName);
