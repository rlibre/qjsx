/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The POSIX functions the newlib of the Arm toolchain lacks on the RP2350
// (see pthread.h and dirent.h of this folder): mutexes and conditions on
// FreeRTOS semaphores, clock_gettime() from the boot timer, and directory and
// file system calls that fail (no file system yet: common/runtime/files.cpp
// is linked, never used while board::dataDirectory() is nullptr).
#include "pthread.h"
#include "dirent.h"

#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "hardware/timer.h"

namespace {

// Value of PTHREAD_MUTEX_INITIALIZER and PTHREAD_COND_INITIALIZER (newlib):
// no semaphore created yet.
const uint32_t NOT_CREATED = 0xFFFFFFFF;

/**
 * The FreeRTOS mutex behind 'mutex', created on its first use. Two tasks may
 * race to create it: the loser deletes its own and takes the winner's.
 * nullptr if out of memory.
 */
SemaphoreHandle_t mutexHandle(pthread_mutex_t *mutex) {
    // already created
    uint32_t current = __atomic_load_n(mutex, __ATOMIC_ACQUIRE);
    if (current != NOT_CREATED) {
        return (SemaphoreHandle_t)(uintptr_t)current;
    }

    // create one, publish it unless another task was faster
    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (created == nullptr) {
        return nullptr;
    }

    uint32_t expected = NOT_CREATED;
    uint32_t value = (uint32_t)(uintptr_t)created;
    bool published = __atomic_compare_exchange_n(mutex, &expected, value, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    if (!published) {
        vSemaphoreDelete(created);
        return (SemaphoreHandle_t)(uintptr_t)expected;
    }

    return created;
}

/**
 * Releases 'mutex', waits for 'condition' at most 'ticks', takes 'mutex'
 * again. 0, or ETIMEDOUT.
 */
int waitCondition(pthread_cond_t *condition, pthread_mutex_t *mutex, TickType_t ticks) {
    if (*condition == NOT_CREATED) {
        return EINVAL;
    }

    // a signal between the unlock and the take leaves the semaphore given:
    // the take then returns at once, no wake up is lost
    SemaphoreHandle_t wakeUp = (SemaphoreHandle_t)(uintptr_t)*condition;
    pthread_mutex_unlock(mutex);
    bool woken = xSemaphoreTake(wakeUp, ticks) == pdTRUE;
    pthread_mutex_lock(mutex);

    return woken ? 0 : ETIMEDOUT;
}

} // namespace

// ### pthread #################################################################

/**
 * Takes the mutex (created on the first call), waiting as long as needed.
 */
extern "C" int pthread_mutex_lock(pthread_mutex_t *mutex) {
    SemaphoreHandle_t handle = mutexHandle(mutex);
    if (handle == nullptr) {
        return ENOMEM;
    }

    xSemaphoreTake(handle, portMAX_DELAY);
    return 0;
}

/**
 * Releases the mutex.
 */
extern "C" int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    SemaphoreHandle_t handle = mutexHandle(mutex);
    if (handle == nullptr) {
        return ENOMEM;
    }

    xSemaphoreGive(handle);
    return 0;
}

/**
 * Creates the condition: a binary semaphore (attributes ignored).
 */
extern "C" int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *) {
    SemaphoreHandle_t wakeUp = xSemaphoreCreateBinary();
    if (wakeUp == nullptr) {
        return ENOMEM;
    }

    *condition = (pthread_cond_t)(uintptr_t)wakeUp;
    return 0;
}

/**
 * Deletes the condition's semaphore.
 */
extern "C" int pthread_cond_destroy(pthread_cond_t *condition) {
    if (*condition != NOT_CREATED) {
        vSemaphoreDelete((SemaphoreHandle_t)(uintptr_t)*condition);
        *condition = NOT_CREATED;
    }

    return 0;
}

/**
 * Waits for a signal, as long as needed.
 */
extern "C" int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex) {
    return waitCondition(condition, mutex, portMAX_DELAY);
}

/**
 * Waits for a signal until 'deadline' (clock_gettime's clock). ETIMEDOUT
 * when it passes first.
 */
extern "C" int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex, const struct timespec *deadline) {
    // the time left, in milliseconds rounded up (none if already past)
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t leftNs = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000000000 + (deadline->tv_nsec - now.tv_nsec);
    int64_t leftMs = leftNs > 0 ? (leftNs + 999999) / 1000000 : 0;

    // as ticks, rounded up, below portMAX_DELAY (which means forever)
    int64_t ticks = (leftMs * configTICK_RATE_HZ + 999) / 1000;
    int64_t longest = (int64_t)portMAX_DELAY - 1;
    TickType_t wait = (TickType_t)(ticks < longest ? ticks : longest);

    return waitCondition(condition, mutex, wait);
}

/**
 * Wakes the waiter of the condition (or the next one to wait).
 */
extern "C" int pthread_cond_signal(pthread_cond_t *condition) {
    if (*condition == NOT_CREATED) {
        return EINVAL;
    }

    xSemaphoreGive((SemaphoreHandle_t)(uintptr_t)*condition);
    return 0;
}

// ### clock ###################################################################

/**
 * Time since boot (64-bit microsecond timer), whatever the clock asked:
 * there is no wall clock yet.
 */
extern "C" int clock_gettime(clockid_t, struct timespec *now) {
    uint64_t microseconds = time_us_64();
    now->tv_sec = (time_t)(microseconds / 1000000);
    now->tv_nsec = (long)(microseconds % 1000000) * 1000;
    return 0;
}

// ### dirent ##################################################################

/**
 * No file system: always fails with ENOSYS.
 */
extern "C" DIR *opendir(const char *) {
    errno = ENOSYS;
    return nullptr;
}

/**
 * Never called (opendir never succeeds).
 */
extern "C" struct dirent *readdir(DIR *) {
    errno = EBADF;
    return nullptr;
}

/**
 * Never called (opendir never succeeds).
 */
extern "C" int closedir(DIR *) {
    errno = EBADF;
    return -1;
}

// ### File system calls #######################################################

// newlib's stat(), unlink(), rename() call these system calls, which the
// pico-sdk does not provide; mkdir(), rmdir(), getcwd() have no newlib
// version at all. No file system: they all fail with ENOSYS.

/**
 * stat() of newlib.
 */
extern "C" int _stat(const char *, struct stat *) {
    errno = ENOSYS;
    return -1;
}

/**
 * unlink() of newlib.
 */
extern "C" int _unlink(const char *) {
    errno = ENOSYS;
    return -1;
}

/**
 * link() of newlib (its rename() is link + unlink).
 */
extern "C" int _link(const char *, const char *) {
    errno = ENOSYS;
    return -1;
}

/**
 * Creates a directory.
 */
extern "C" int mkdir(const char *, mode_t) {
    errno = ENOSYS;
    return -1;
}

/**
 * Deletes an empty directory.
 */
extern "C" int rmdir(const char *) {
    errno = ENOSYS;
    return -1;
}

/**
 * The current directory.
 */
extern "C" char *getcwd(char *, size_t) {
    errno = ENOSYS;
    return nullptr;
}
