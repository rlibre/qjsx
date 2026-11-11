// Constants of the ESP32 board shared by main.cpp (which creates the JS task)
// and board_esp32.cpp (which gives QuickJS its stack limit).
#pragma once

#include <stddef.h>

namespace board {

const size_t JS_TASK_STACK = 48 * 1024;   // bytes of the JS task's stack, internal RAM
const size_t JS_STACK_LIMIT = 40 * 1024;  // QuickJS's limit: leaves room for the C code under JS
const int    JS_TASK_PRIORITY = 5;        // like the other application tasks
const int    JS_TASK_CORE = 1;            // core 0 keeps the IDF tasks (WiFi later)

} // namespace board
