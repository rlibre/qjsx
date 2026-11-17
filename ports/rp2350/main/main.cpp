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
#include "hardware/watchdog.h"
#include "pico/time.h"
#include <string.h>

namespace {

#ifdef QJSX_FLASH_TRACE
// Measure (temporary, QJSX_FLASH_TRACE): the watchdog restarts a hung board,
// fed by a task of core 0; after such a restart, the trace of the last flash
// operation (board_rp2350.cpp) is printed for a minute.
const uint32_t WATCHDOG_MS = 3000;   // a board silent that long restarts
const uint32_t FEED_MS = 200;        // between two feeds
const int REPORT_EVERY = 10;         // feeds between two prints of the report
const int REPORT_TIMES = 30;         // prints of the report

bool hungBefore = false;             // the last restart came from the watchdog
uint32_t lastCount = 0;              // flash operations before it
uint32_t lastStep = 0;               // the step of the last one (99: fatal())
uint32_t lastResult = 0;             // the last result of flash_safe_execute
uint32_t lastReader = 0;             // the phase of the USB reader (1 waiting, 2 reading, 3 in the agent)
uint32_t faultCore = 0;              // a hard fault: 1 + its core; 0: none
uint32_t faultPc = 0;                // the instruction that faulted
uint32_t faultLr = 0;                // and the return address of its function
uint32_t lastUsbFree = 0;            // least free stack of the USB reader before the restart (words)
uint32_t lastJsFree = 0;             // the same for the JS task

// The least free stacks (magic, USB reader, JS task), kept in RAM that the
// start-up does not clear
const uint32_t STACK_MAGIC = 0x57ac4ed0;
uint32_t __uninitialized_ram(stackRecord)[3];

// The tasks running on each core when the feeder stopped (magic, then 8
// characters per core), seen by a timer interrupt of core 0
const uint32_t STUCK_MAGIC = 0x5700c4ed;
const uint32_t STUCK_AFTER_MS = 2000;   // the feeder silent that long: record
const int NAME_CHARS = 8;
uint32_t __uninitialized_ram(stuckRecord)[1 + 2 * NAME_CHARS / 4];
volatile uint32_t lastFeedMs = 0;       // when the feeder last ran (ms since boot)
char stuckNames[2][NAME_CHARS + 1];     // the record of the previous run, as text
bool stuckKnown = false;
repeating_timer_t stuckTimer;

/**
 * Timer interrupt of core 0: once the feeder has been silent STUCK_AFTER_MS,
 * records the names of the tasks running on both cores (plain reads).
 */
bool checkStuck(repeating_timer_t *) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    bool stuck = now - lastFeedMs > STUCK_AFTER_MS;
    if (stuck && stuckRecord[0] != STUCK_MAGIC) {
        char *names = (char *)&stuckRecord[1];
        for (int core = 0; core < 2; core++) {
            TaskHandle_t running = xTaskGetCurrentTaskHandleForCore(core);
            const char *name = running != nullptr ? pcTaskGetName(running) : "?";
            strncpy(names + core * NAME_CHARS, name, NAME_CHARS);
        }
        stuckRecord[0] = STUCK_MAGIC;
    }
    return true;
}

/**
 * Feeds the watchdog; prints the report of the last hang for a minute.
 */
void watchdogTask(void *) {
    TaskHandle_t usbTask = nullptr;   // the reader of the USB link, found by its name
    TaskHandle_t jsTaskHandle = nullptr;

    for (int feeds = 0;; feeds++) {
        watchdog_update();
        lastFeedMs = to_ms_since_boot(get_absolute_time());

        // the smallest free stack seen so far of the USB reader and of the
        // JS task (words), kept across a restart
        if (usbTask == nullptr) {
            usbTask = xTaskGetHandle("agent-usb");
        }
        if (jsTaskHandle == nullptr) {
            jsTaskHandle = xTaskGetHandle("js");
        }
        if (usbTask != nullptr && jsTaskHandle != nullptr) {
            stackRecord[1] = uxTaskGetStackHighWaterMark(usbTask);
            stackRecord[2] = uxTaskGetStackHighWaterMark(jsTaskHandle);
            stackRecord[0] = STACK_MAGIC;
        }

        // the report, every REPORT_EVERY feeds, REPORT_TIMES times
        bool reportDue = hungBefore && feeds % REPORT_EVERY == 0 && feeds / REPORT_EVERY < REPORT_TIMES;
        if (reportDue) {
            printf("qjsx: flash trace: restarted by the watchdog after %lu flash operations, step %lu, last result %ld, USB reader %lu\n",
                   (unsigned long)lastCount, (unsigned long)lastStep, (long)(int32_t)lastResult, (unsigned long)lastReader);
            printf("qjsx: fault trace: core %lu pc %08lx lr %08lx\n",
                   (unsigned long)faultCore, (unsigned long)faultPc, (unsigned long)faultLr);
            printf("qjsx: stack trace: before the restart, least free stack: USB reader %lu words, JS task %lu words\n",
                   (unsigned long)lastUsbFree, (unsigned long)lastJsFree);
            printf("qjsx: stuck trace: %s; core 0 ran '%s', core 1 ran '%s'\n",
                   stuckKnown ? "recorded" : "nothing recorded (core 0 interrupts off?)", stuckNames[0], stuckNames[1]);
        }

        vTaskDelay(pdMS_TO_TICKS(FEED_MS));
    }
}

// A hard fault, kept in RAM that the start-up does not clear (the boot ROM
// rewrites the scratch registers 4 to 7): magic, 1 + core, pc, lr.
const uint32_t FAULT_MAGIC = 0x0fa017ed;
uint32_t __uninitialized_ram(faultRecord)[4];

} // namespace

/**
 * Hard fault (measure, QJSX_FLASH_TRACE): the frame the hardware stacked
 * gives the faulting PC and LR; kept in faultRecord, then the core stops
 * (the watchdog restarts the board).
 */
extern "C" void qjsxRecordHardFault(const uint32_t *frame) {
    faultRecord[1] = 1 + get_core_num();
    faultRecord[2] = frame[6];
    faultRecord[3] = frame[5];
    faultRecord[0] = FAULT_MAGIC;

    for (;;) {
        __asm volatile("wfi");
    }
}

/**
 * The hard fault vector of the SDK: picks the stack the fault was stacked on
 * (bit 2 of EXC_RETURN in LR) and hands it to qjsxRecordHardFault.
 */
extern "C" __attribute__((naked)) void isr_hardfault(void) {
    __asm volatile(
        "movs r0, #4\n"
        "mov r1, lr\n"
        "tst r0, r1\n"
        "beq 1f\n"
        "mrs r0, psp\n"
        "b 2f\n"
        "1:\n"
        "mrs r0, msp\n"
        "2:\n"
        "b qjsxRecordHardFault\n");
}

namespace {

/**
 * Reads the trace left by a restart of the watchdog, clears it, starts the
 * watchdog and its feeder on core 0.
 */
void startFlashTrace() {
    // the trace of the previous run
    hungBefore = watchdog_caused_reboot();
    lastCount = watchdog_hw->scratch[0];
    lastStep = watchdog_hw->scratch[1];
    lastResult = watchdog_hw->scratch[2];
    lastReader = watchdog_hw->scratch[3];
    for (int i = 0; i <= 3; i++) {
        watchdog_hw->scratch[i] = 0;
    }

    // a hard fault before it
    bool faulted = faultRecord[0] == FAULT_MAGIC;
    if (faulted) {
        faultCore = faultRecord[1];
        faultPc = faultRecord[2];
        faultLr = faultRecord[3];
    }
    faultRecord[0] = 0;

    // the stacks before it
    bool stacksKnown = stackRecord[0] == STACK_MAGIC;
    if (stacksKnown) {
        lastUsbFree = stackRecord[1];
        lastJsFree = stackRecord[2];
    }
    stackRecord[0] = 0;

    // the tasks that ran when the feeder stopped
    stuckKnown = stuckRecord[0] == STUCK_MAGIC;
    if (stuckKnown) {
        const char *names = (const char *)&stuckRecord[1];
        for (int core = 0; core < 2; core++) {
            memcpy(stuckNames[core], names + core * NAME_CHARS, NAME_CHARS);
            stuckNames[core][NAME_CHARS] = 0;
        }
    }
    stuckRecord[0] = 0;

    // the check of the feeder, from a timer interrupt of core 0
    lastFeedMs = to_ms_since_boot(get_absolute_time());
    add_repeating_timer_ms(500, &checkStuck, nullptr, &stuckTimer);

    // the watchdog, fed on core 0
    watchdog_enable(WATCHDOG_MS, true);
    platform::ThreadOptions options = { "watchdog", platform::DEFAULT_STACK, 1, 0 };
    platform::startThread(&watchdogTask, nullptr, options);
}
#endif

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

#ifdef QJSX_FLASH_TRACE
    // measure (temporary): restart a hung board, say where it hung
    startFlashTrace();
#endif

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
