/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The way into the Teensy's loader (see loader_imxrt1062.h): a one-shot of
// the PIT's channel 2, whose interrupt halts the core with "bkpt #251".
#include "loader_imxrt1062.h"

#include "fsl_pit.h"

namespace {

const uint32_t PIT_HZ = 24000000;                      // PERCLK: the crystal (clock_imxrt1062.c)
const uint32_t PIT_TICKS_PER_MS = PIT_HZ / 1000;        // PIT ticks in a millisecond
const uint32_t LOADER_INTERRUPT_PRIORITY = 2;          // above everything FreeRTOS masks

} // namespace

namespace loader {

/**
 * 1. channel 2 counts ENTER_DELAY_MS once, then interrupts;
 * 2. its interrupt (PIT_IRQHandler) enters the loader.
 */
void enterSoon() {
    // the delay
    PIT_StopTimer(PIT, kPIT_Chnl_2);
    PIT_SetTimerPeriod(PIT, kPIT_Chnl_2, ENTER_DELAY_MS * PIT_TICKS_PER_MS);
    PIT_ClearStatusFlags(PIT, kPIT_Chnl_2, kPIT_TimerFlag);

    // its interrupt, then the count
    PIT_EnableInterrupts(PIT, kPIT_Chnl_2, kPIT_TimerInterruptEnable);
    NVIC_SetPriority(PIT_IRQn, LOADER_INTERRUPT_PRIORITY);
    EnableIRQ(PIT_IRQn);
    PIT_StartTimer(PIT, kPIT_Chnl_2);
}

} // namespace loader

/**
 * The PIT's interrupt (every channel shares it; only channel 2 interrupts):
 * halts the core for the loader chip. Does not return.
 */
extern "C" void PIT_IRQHandler(void) {
    // acknowledged, then the breakpoint the loader chip waits for, forever
    PIT_ClearStatusFlags(PIT, kPIT_Chnl_2, kPIT_TimerFlag);

    for (;;) {
        __asm volatile("bkpt #251");
    }
}
