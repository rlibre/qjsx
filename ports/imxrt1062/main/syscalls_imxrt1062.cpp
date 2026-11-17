/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The system calls newlib needs on the i.MX RT1062 (no SDK runtime provides
// them, unlike the pico-sdk): the C heap (_sbrk, in the OCRAM after the JS
// heap, imxrt1062.ld), the lock of malloc (FreeRTOS tasks), the console
// (stdout / stderr on the USB serial port), the clock, and failures for what
// a board without file system cannot do. The calls of the file system that
// the RP2350 lacks too (_stat, _unlink, _link, mkdir...) are in
// ports/freertos/compat/posix_compat.cpp.
#include "platform.h"
#include "usb_serial_imxrt1062.h"

#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "FreeRTOS.h"
#include "task.h"
#include "fsl_device_registers.h"

// from imxrt1062.ld: the C heap
extern "C" uint8_t __qjsx_c_heap_start[];
extern "C" uint8_t __qjsx_c_heap_end[];

namespace {

uint8_t *heapBreak = __qjsx_c_heap_start;   // end of the C heap taken so far

const int STDIN = 0;    // the console descriptors
const int STDOUT = 1;   // to the USB serial port
const int STDERR = 2;   // to the USB serial port

/**
 * True once the scheduler started (running, or suspended by someone: heap_3
 * suspends it around malloc, whose lock then nests).
 */
bool schedulerStarted() {
    return xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}

} // namespace

/**
 * The end of the C heap taken so far (board_imxrt1062.cpp: memoryStatus).
 */
extern "C" uint8_t *qjsxHeapBreak() {
    return heapBreak;
}

/**
 * Grows the C heap by 'increment' bytes; (void *)-1 and ENOMEM past the end
 * of the OCRAM.
 */
extern "C" void *_sbrk(ptrdiff_t increment) {
    uint8_t *previous = heapBreak;
    bool pastEnd = heapBreak + increment > __qjsx_c_heap_end;
    if (pastEnd) {
        errno = ENOMEM;
        return (void *)-1;
    }

    heapBreak += increment;
    return previous;
}

/**
 * malloc's lock: the scheduler suspended while malloc works (suspensions
 * nest, each one resumed by __malloc_unlock; never from an interrupt).
 * Nothing to lock before the scheduler starts: one thread.
 */
extern "C" void __malloc_lock(struct _reent *) {
    if (schedulerStarted()) {
        vTaskSuspendAll();
    }
}

/**
 * Releases malloc's lock (the same condition as __malloc_lock: the
 * scheduler cannot start inside malloc).
 */
extern "C" void __malloc_unlock(struct _reent *) {
    if (schedulerStarted()) {
        xTaskResumeAll();
    }
}

/**
 * stdout and stderr go to the USB serial port; other descriptors fail.
 */
extern "C" int _write(int file, const char *data, int length) {
    bool isOutput = file == STDOUT || file == STDERR;
    if (!isOutput) {
        errno = EBADF;
        return -1;
    }

    usbserial::write((const uint8_t *)data, (size_t)length);
    return length;
}

/**
 * No input on stdin (the agent reads the USB itself).
 */
extern "C" int _read(int, char *, int) {
    errno = ENOSYS;
    return -1;
}

/**
 * No file system: no file opens.
 */
extern "C" int _open(const char *, int, int) {
    errno = ENOSYS;
    return -1;
}

/**
 * Nothing to close.
 */
extern "C" int _close(int) {
    errno = EBADF;
    return -1;
}

/**
 * No seekable file.
 */
extern "C" int _lseek(int, int, int) {
    errno = ESPIPE;
    return -1;
}

/**
 * The console descriptors are character devices (unbuffered by newlib).
 */
extern "C" int _fstat(int file, struct stat *status) {
    bool isConsole = file == STDIN || file == STDOUT || file == STDERR;
    if (!isConsole) {
        errno = EBADF;
        return -1;
    }

    status->st_mode = S_IFCHR;
    return 0;
}

/**
 * The console descriptors are terminals.
 */
extern "C" int _isatty(int file) {
    return file == STDIN || file == STDOUT || file == STDERR;
}

/**
 * exit() of a C program: there is no other program, the chip restarts.
 */
extern "C" void _exit(int) {
    NVIC_SystemReset();
}

/**
 * No processes.
 */
extern "C" int _kill(int, int) {
    errno = EINVAL;
    return -1;
}

/**
 * The only process.
 */
extern "C" int _getpid(void) {
    return 1;
}

/**
 * Time since boot (platform::monotonicMs): there is no wall clock yet.
 */
extern "C" int _gettimeofday(struct timeval *now, void *) {
    int64_t milliseconds = platform::monotonicMs();
    now->tv_sec = (time_t)(milliseconds / 1000);
    now->tv_usec = (suseconds_t)(milliseconds % 1000) * 1000;
    return 0;
}
