/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// FreeRTOS configuration of the i.MX RT1062 firmware: one Cortex-M7 core
// (port GCC/ARM_CM4F of FreeRTOS-Kernel: the RT1062's M7 is a revision
// r1p1, which does not need the ARM_CM7/r0p1 port), task memory from malloc
// (heap_3, the C heap in the OCRAM). The USB interrupt gives FreeRTOS
// semaphores: its priority is below configMAX_SYSCALL_INTERRUPT_PRIORITY.
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

// the core's clock, set by clock_imxrt1062.c (C only: not for the assembler)
#if defined(__GNUC__) && !defined(__ASSEMBLER__)
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern uint32_t SystemCoreClock;
#ifdef __cplusplus
}
#endif
#endif

// ### Scheduler ###############################################################

#define configUSE_PREEMPTION                    1
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      (SystemCoreClock)
#define configTICK_RATE_HZ                      ((TickType_t)1000)   // 1 ms tick, as on the other ports
#define configMAX_PRIORITIES                    32
#define configMINIMAL_STACK_SIZE                ((configSTACK_DEPTH_TYPE)256)   // words
#define configMAX_TASK_NAME_LEN                 16
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TIME_SLICING                  1
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0

// ### Synchronization #########################################################

#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1   // platform::Mutex
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_QUEUE_SETS                    0
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_TASK_NOTIFICATIONS            1
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 1   // the thread_local block of a task (tls_imxrt1062.h)

// ### Memory ##################################################################

#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   (64 * 1024)   // unused by heap_3 (malloc)
#define configAPPLICATION_ALLOCATED_HEAP        0

// failures end in platform::fatal (platform_imxrt1062.cpp)
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1

// ### Timers, tools ###########################################################

#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            512   // words
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1
#define configUSE_APPLICATION_TASK_TAG          0
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     1

// ### Interrupt priorities (4 bits on the RT1062) #############################

#define configPRIO_BITS                                 4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5   // interrupts at 5..15 may call FromISR functions
#define configKERNEL_INTERRUPT_PRIORITY         (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

// the port's handlers under the names of the vector table (startup_MIMXRT1062.S)
#define vPortSVCHandler                         SVC_Handler
#define xPortPendSVHandler                      PendSV_Handler
#define xPortSysTickHandler                     SysTick_Handler

#include <assert.h>
#define configASSERT(condition)                 assert(condition)

// ### API included ############################################################

#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_xTaskResumeFromISR              1
#define INCLUDE_xQueueGetMutexHolder            1

#endif // FREERTOS_CONFIG_H
