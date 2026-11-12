/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Entry point of the RP2350 firmware: starts the USB stack, creates the JS
// task, which runs the device (agent, debugger stub, installed package) as
// `qjsx agent` does on the desktop, then hands the cores to FreeRTOS.
#include "board_rp2350.h"
#include "device_modules.h"
#include "modules.h"
#include "version.h"
#include "platform.h"
#include "quickjs.h"
#include "runner.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdio.h"

namespace {

/**
 * The JS task: becomes the device's JS thread, for good.
 */
void jsTask(void *) {
    // the agent's identity in HELLO; no token on a USB link
    agent::Identity identity = { "qjsx rp2350", CONFIG_VERSION, nullptr, JS_GetBytecodeVersion(), QJSX_VERSION };
    agent::runDevice(identity);

    // runDevice returns only if the links could not start
    printf("qjsx: the device could not start\n");
}

} // namespace

/**
 * 1. the PSRAM and the JS heap in it, while nothing else runs;
 * 2. the USB device (stdio_usb: console and host link), serviced from now on
 *    by an interrupt of the pico-sdk;
 * 3. the hardware modules of this port, before any program runs;
 * 4. the JS task with its large stack, on core 1;
 * 5. the FreeRTOS scheduler on both cores (does not return).
 */
int main() {
    // the PSRAM
    board::startMemory();

    // the USB device
    stdio_init_all();

    // the hardware modules
    rt::registerNativeModule("device/gpio", &jsNewModule_gpio);

    // the JS task
    platform::ThreadOptions options = { "js", board::JS_TASK_STACK, board::JS_TASK_PRIORITY, board::JS_TASK_CORE };
    if (!platform::startThread(&jsTask, nullptr, options)) {
        printf("qjsx: cannot start the JS task\n");
    }

    // the scheduler
    vTaskStartScheduler();
    return 0;
}
