/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Entry point of the i.MX RT1062 firmware: sets the chip up (clocks, caches,
// time, JS heap, flash driver, USB), creates the JS task, which runs the
// device (agent, debugger stub, installed package) as `qjsx agent` does on
// the desktop, then hands the core to FreeRTOS.
#include "board_imxrt1062.h"
#include "clock_imxrt1062.h"
#include "flash_imxrt1062.h"
#include "platform_imxrt1062.h"
#include "usb_serial_imxrt1062.h"
#include "device_modules.h"
#include "modules.h"
#include "platform.h"
#include "quickjs.h"
#include "runner.h"
#include "version.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "fsl_device_registers.h"

namespace {

/**
 * The JS task: becomes the device's JS thread, for good.
 */
void jsTask(void *) {
    // the agent's identity in HELLO; no token on a USB link
    agent::Identity identity = { "qjsx imxrt1062", CONFIG_VERSION, nullptr, JS_GetBytecodeVersion(), QJSX_VERSION };
    agent::runDevice(identity);

    // runDevice returns only if the links could not start
    printf("qjsx: the device could not start\n");
}

} // namespace

/**
 * 1. the clocks at run speed, the data cache on (SystemInit turned the
 *    instruction cache on);
 * 2. the 64-bit time, the JS heap, the ROM's flash driver;
 * 3. the USB device (console and host link), serviced by its interrupt from
 *    now on;
 * 4. the hardware modules of this port;
 * 5. the JS task with its large stack;
 * 6. the FreeRTOS scheduler (does not return).
 */
int main() {
    // clocks, caches
    qjsxStartClocks();
    SCB_EnableDCache();

    // time, JS heap, flash
    platform::startClock();
    board::startMemory();
    bool flashReady = flash::start();

    // the USB device
    usbserial::start();
    if (!flashReady) {
        printf("qjsx: the flash driver of the ROM refused the configuration: no packages\n");
    }

    // the hardware modules, before any program runs
    rt::registerNativeModule("device/gpio", &jsNewModule_gpio);
    rt::registerNativeModule("device/board", &jsNewModule_board);
    rt::registerNativeModule("device/pwm", &jsNewModule_pwm);

    // the JS task
    platform::ThreadOptions options = { "js", board::JS_TASK_STACK, board::JS_TASK_PRIORITY, platform::ANY_CORE };
    if (!platform::startThread(&jsTask, nullptr, options)) {
        printf("qjsx: cannot start the JS task\n");
    }

    // the scheduler
    vTaskStartScheduler();
    return 0;
}
