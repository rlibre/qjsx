/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/* The part of <pthread.h> the QuickJS engine uses (the lock of the class ids,
 * Atomics.wait / Atomics.notify), for the ports built with the newlib of the
 * Arm toolchain (RP2350, i.MX RT1062): the newlib of the Arm
 * toolchain has the pthread types but no thread functions. Implemented on
 * FreeRTOS in posix_compat.cpp. This folder comes before the system headers
 * in the include path, so this file replaces the newlib one.
 *
 * Mutexes are created on their first lock (PTHREAD_MUTEX_INITIALIZER is a
 * constant). A condition holds one waiter at a time, which is how the engine
 * uses them (one condition per waiting thread). clock_gettime() counts from
 * the boot: there is no wall clock yet. */
#ifndef QJSX_COMPAT_PTHREAD_H
#define QJSX_COMPAT_PTHREAD_H

#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* newlib's types: 32-bit handles, here the FreeRTOS semaphore; the
 * initializer value means "not created yet" */
#define PTHREAD_MUTEX_INITIALIZER _PTHREAD_MUTEX_INITIALIZER
#define PTHREAD_COND_INITIALIZER  _PTHREAD_COND_INITIALIZER

/**
 * Takes the mutex (created on the first call), waiting as long as needed.
 * 0, or ENOMEM when its semaphore cannot be created.
 */
int pthread_mutex_lock(pthread_mutex_t *mutex);

/**
 * Releases the mutex. 0, or ENOMEM when its semaphore cannot be created.
 */
int pthread_mutex_unlock(pthread_mutex_t *mutex);

/**
 * Creates the condition (attributes ignored). 0, or ENOMEM.
 */
int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes);

/**
 * Deletes the condition. Always 0.
 */
int pthread_cond_destroy(pthread_cond_t *condition);

/**
 * Releases 'mutex', waits for a signal as long as needed, takes 'mutex'
 * again. 0, or EINVAL for a condition never created.
 */
int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex);

/**
 * As pthread_cond_wait, until 'deadline' (clock_gettime's clock). ETIMEDOUT
 * when it passes first.
 */
int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex, const struct timespec *deadline);

/**
 * Wakes the waiter of the condition (or the next one to wait). 0, or EINVAL
 * for a condition never created.
 */
int pthread_cond_signal(pthread_cond_t *condition);

/* newlib declares these only with _POSIX_TIMERS */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME ((clockid_t)1)
#endif

/**
 * Time since boot, whatever the clock asked: there is no wall clock yet.
 * Always 0.
 */
int clock_gettime(clockid_t clock, struct timespec *now);

#ifdef __cplusplus
}
#endif

#endif /* QJSX_COMPAT_PTHREAD_H */
