/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Measure (temporary, QJSX_NATIVE_PROFILER): the native profiler of the
// i.MX RT1062 (platform.h nativeProfileStart / nativeProfileStop).
//
// The general purpose timer GPT1 interrupts the core every interval. Its
// handler records where the JS thread was: when the interrupt came from
// thread mode (no other exception active) while the JS task ran, the address
// the hardware stacked on the task's stack (the PC of the exception frame);
// else a marker (another task, an interrupt handler). The samples are named
// on the PC with addr2line and the firmware's qjsx.elf.
#ifdef QJSX_NATIVE_PROFILER

#include "platform.h"
#include "memory.h"

#include "fsl_common.h"
#include "FreeRTOS.h"
#include "task.h"

namespace {

const uint32_t GPT_CLOCK_HZ = 150000000;          // ipg_clk (clock_imxrt1062.c), source 1 of the GPT
const uint32_t PROFILER_INTERRUPT_PRIORITY = 2;   // above what FreeRTOS masks: samples critical sections too
const int STACKED_PC = 6;                         // words before the PC in an exception frame (r0-r3, r12, lr)

uint32_t *samples = nullptr;                      // memory::allocate'd; written by the interrupt handler
volatile int sampleCount = 0;                     // samples written
int sampleCapacity = 0;                           // samples the buffer holds
TaskHandle_t profiledTask = nullptr;              // the JS task, set by nativeProfileStart

/**
 * The sample of this interrupt: the address where the JS task was, or a
 * marker when it was not the one interrupted.
 */
uint32_t currentSample() {
    // interrupted an interrupt handler: the task's stack says nothing
    bool fromThreadMode = (SCB->ICSR & SCB_ICSR_RETTOBASE_Msk) != 0;
    if (!fromThreadMode) {
        return platform::NATIVE_SAMPLE_INTERRUPT;
    }

    // another task ran (the current one's handle is a plain read)
    bool jsTaskRan = xTaskGetCurrentTaskHandle() == profiledTask;
    if (!jsTaskRan) {
        return platform::NATIVE_SAMPLE_OTHER_TASK;
    }

    // the JS task runs on its own stack (PSP): the hardware stacked its PC
    const uint32_t *frame = (const uint32_t *)__get_PSP();
    return frame[STACKED_PC];
}

} // namespace

/**
 * GPT1 interrupt: one sample, while the buffer has room.
 */
extern "C" void GPT1_IRQHandler(void) {
    // acknowledged first (the write must reach the timer before the return)
    GPT1->SR = GPT_SR_OF1_MASK;

    // the sample, while there is room
    if (sampleCount < sampleCapacity) {
        samples[sampleCount] = currentSample();
        sampleCount = sampleCount + 1;
    }

    __DSB();
}

namespace platform {

/**
 * Starts GPT1 and the sampling of the calling (JS) task (see platform.h).
 */
bool nativeProfileStart(int intervalUs, int capacity) {
    // the previous samples are dropped; the new buffer
    nativeProfileStop(nullptr);
    memory::release(samples);
    samples = (uint32_t *)memory::allocate((size_t)capacity * sizeof(uint32_t));
    sampleCapacity = capacity;
    sampleCount = 0;
    profiledTask = xTaskGetCurrentTaskHandle();

    // GPT1 clocked, reset, from ipg_clk without divider, restarting at its
    // compare value (one interrupt per interval)
    CCM->CCGR1 |= CCM_CCGR1_CG10_MASK | CCM_CCGR1_CG11_MASK;
    GPT1->CR = 0;
    GPT1->CR = GPT_CR_SWR_MASK;

    while ((GPT1->CR & GPT_CR_SWR_MASK) != 0) {
    }

    uint32_t ticks = (uint32_t)((uint64_t)GPT_CLOCK_HZ * (uint64_t)intervalUs / 1000000u);
    GPT1->PR = 0;
    GPT1->OCR[0] = ticks - 1;
    GPT1->SR = GPT_SR_OF1_MASK;
    GPT1->IR = GPT_IR_OF1IE_MASK;

    // its interrupt, then the count
    NVIC_SetPriority(GPT1_IRQn, PROFILER_INTERRUPT_PRIORITY);
    EnableIRQ(GPT1_IRQn);
    GPT1->CR = GPT_CR_CLKSRC(1) | GPT_CR_ENMOD_MASK | GPT_CR_EN_MASK;
    return true;
}

/**
 * Stops GPT1; the samples stay until the next start (see platform.h).
 */
const uint32_t *nativeProfileStop(int *count) {
    // no more interrupt
    GPT1->CR = 0;
    DisableIRQ(GPT1_IRQn);
    GPT1->SR = GPT_SR_OF1_MASK;

    if (count != nullptr) {
        *count = sampleCount;
    }
    return samples;
}

} // namespace platform

#endif
