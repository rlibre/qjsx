/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// FreeRTOS configuration of the RP2350 firmware: SMP on both ARM cores (port
// RP2350_ARM_NTZ of FreeRTOS-Kernel-Community-Supported-Ports), interop with
// the pico-sdk's mutexes, semaphores and sleeps, task memory from malloc
// (heap_3). The JS task is pinned to core 1, the links and the USB stack run
// on core 0 (main/board_rp2350.h).
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

// ### Scheduler ###############################################################

#define configUSE_PREEMPTION                    1
#define configUSE_TICKLESS_IDLE                 0   // not tested on this port
#define configUSE_IDLE_HOOK                     0
#define configUSE_PASSIVE_IDLE_HOOK             0
#define configUSE_TICK_HOOK                     0
#define configTICK_RATE_HZ                      ((TickType_t)1000)   // 1 ms tick, as on the ESP32
#define configMAX_PRIORITIES                    32
#define configMINIMAL_STACK_SIZE                ((configSTACK_DEPTH_TYPE)512)   // words
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TIME_SLICING                  1
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t

// ### Both cores ##############################################################

#define configNUMBER_OF_CORES                   2
#define configTICK_CORE                         0
#define configRUN_MULTIPLE_PRIORITIES           1
#define configUSE_CORE_AFFINITY                 1   // tasks pinned to a core (also needed by flash_safe_execute)

// ### Synchronization #########################################################

#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1   // platform::Mutex
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_QUEUE_SETS                    1
#define configQUEUE_REGISTRY_SIZE               8
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 5
#define configUSE_TASK_NOTIFICATIONS            1

// pico-sdk mutexes / semaphores / sleeps block the FreeRTOS task, not the core
#define configSUPPORT_PICO_SYNC_INTEROP         1
#define configSUPPORT_PICO_TIME_INTEROP         1

// ### Memory ##################################################################

#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   (64 * 1024)   // unused by heap_3 (malloc)
#define configAPPLICATION_ALLOCATED_HEAP        0

// failures end in platform::fatal (platform_rp2350.cpp)
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1

// ### Timers, tools ###########################################################

#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            1024   // words
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1
#define configUSE_APPLICATION_TASK_TAG          0
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     1

// ### Armv8-M (values required by the port, see its README) ###################

#define configENABLE_FPU                        1   // the FPU state is saved with each task
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16

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
