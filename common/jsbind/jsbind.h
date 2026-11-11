/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// jsbind - C++17 bindings for QuickJS (header only).
//
// No C++ standard library, no C++ exceptions, no RTTI: usable on the ESP32
// as well as on the desktop. Compile with -std=c++17 (gcc or clang:
// __COUNTER__ and computed-free code only).
//
// Overview (details in each header):
//   jsbind_error.h    errors: "the first error wins", js::fail(), js::failSystem()
//   jsbind_value.h    js::Value, js::Function, js::Str, js::Bytes, js::Buffer,
//                     js::Optional, js::Range, js::Args
//   jsbind_convert.h  JS <-> C++ conversions, JS_ENUM, JS_STRUCT
//   jsbind_class.h    JS_CLASS, JS_EXPORT, init(), js::keepAlive(), js::create()
//   jsbind_promise.h  js::Promise<T>, js::Resolver<T>
//   jsbind_module.h   JS_MODULE, JS_GLOBALS, JS_FUNCTION, JS_ADD_CLASS
#pragma once

#include "jsbind_base.h"
#include "jsbind_error.h"
#include "jsbind_value.h"
#include "jsbind_convert.h"
#include "jsbind_class.h"
#include "jsbind_promise.h"
#include "jsbind_module.h"
