/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/* The part of <pthread.h> the QuickJS engine uses (the lock of the class ids,
 * Atomics.wait / Atomics.notify), for the RP2350: the newlib of the Arm
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

int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes);
int pthread_cond_destroy(pthread_cond_t *condition);
int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex);
int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex, const struct timespec *deadline);
int pthread_cond_signal(pthread_cond_t *condition);

/* newlib declares these only with _POSIX_TIMERS */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME ((clockid_t)1)
#endif
int clock_gettime(clockid_t clock, struct timespec *now);

#ifdef __cplusplus
}
#endif

#endif /* QJSX_COMPAT_PTHREAD_H */
