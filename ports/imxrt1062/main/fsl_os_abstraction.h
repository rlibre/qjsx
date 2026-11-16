/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/* Our own "OS abstraction" (OSA) for NXP's USB device stack. NXP's OSA is in
 * its mcux-component repository, whose license is NXP proprietary: not used
 * (CLAUDE.md). The stack runs in bare-metal mode (USB_DEVICE_CONFIG_USE_TASK
 * 0, everything in the USB interrupt), where it only needs:
 *   - critical sections (OSA_SR_ALLOC, OSA_ENTER_CRITICAL, OSA_EXIT_CRITICAL):
 *     all interrupts masked, the previous mask restored, usable from a task
 *     and from an interrupt;
 *   - OSA_MutexCreate / OSA_MutexDestroy, called by the CDC class for its
 *     pipes, which it never locks in this mode: they do nothing.
 * Anything else the stack would ask fails to compile, on purpose. */
#ifndef QJSX_FSL_OS_ABSTRACTION_H
#define QJSX_FSL_OS_ABSTRACTION_H

#include <stdint.h>

#include "fsl_device_registers.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result of an OSA call. */
typedef enum _osa_status {
    KOSA_StatusSuccess = 0,   /* done */
    KOSA_StatusError = 1,     /* failed */
    KOSA_StatusTimeout = 2,   /* gave up waiting */
    KOSA_StatusIdle = 3,      /* nothing to do */
} osa_status_t;

/* A mutex of the stack: unused (see above). */
typedef void *osa_mutex_handle_t;

/* bytes the stack reserves per mutex */
#define OSA_MUTEX_HANDLE_SIZE 4u

/* the saved interrupt mask of a critical section, in the caller's frame */
#define OSA_SR_ALLOC() uint32_t osaCurrentSr = 0u

/* masks every interrupt, keeps the previous mask */
#define OSA_ENTER_CRITICAL() (osaCurrentSr = qjsxOsaEnterCritical())

/* restores the mask kept by OSA_ENTER_CRITICAL */
#define OSA_EXIT_CRITICAL() qjsxOsaExitCritical(osaCurrentSr)

/**
 * Masks every interrupt; returns the previous PRIMASK, for
 * qjsxOsaExitCritical. Nests (an inner section restores a masked state).
 */
static inline uint32_t qjsxOsaEnterCritical(void) {
    uint32_t previous = __get_PRIMASK();
    __disable_irq();
    return previous;
}

/**
 * Restores the PRIMASK of qjsxOsaEnterCritical.
 */
static inline void qjsxOsaExitCritical(uint32_t previous) {
    __set_PRIMASK(previous);
}

/**
 * Creates a mutex of the stack: nothing to create in bare-metal mode.
 */
static inline osa_status_t OSA_MutexCreate(osa_mutex_handle_t mutex) {
    (void)mutex;
    return KOSA_StatusSuccess;
}

/**
 * Destroys a mutex of OSA_MutexCreate: nothing to do.
 */
static inline osa_status_t OSA_MutexDestroy(osa_mutex_handle_t mutex) {
    (void)mutex;
    return KOSA_StatusSuccess;
}

#ifdef __cplusplus
}
#endif

#endif /* QJSX_FSL_OS_ABSTRACTION_H */
