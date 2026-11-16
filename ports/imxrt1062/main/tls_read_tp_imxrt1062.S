/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/* __aeabi_read_tp: the compiler's way to find the thread_local block of the
 * calling task (tls_imxrt1062.h). The ABI lets it change r0 only: the
 * registers a C function may change (r1-r3, r12) are kept around the call of
 * qjsxThreadPointer (tls_imxrt1062.cpp), lr with them; r4 only keeps the
 * stack 8-byte aligned for the call (an even number of registers). */
    .syntax unified
    .thumb
    .text
    .global __aeabi_read_tp
    .type __aeabi_read_tp, %function
    .thumb_func
__aeabi_read_tp:
    push    {r1, r2, r3, r4, r12, lr}
    bl      qjsxThreadPointer
    pop     {r1, r2, r3, r4, r12, pc}
    .size __aeabi_read_tp, . - __aeabi_read_tp
