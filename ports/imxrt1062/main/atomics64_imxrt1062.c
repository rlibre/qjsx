/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/* The 64-bit atomic operations GCC calls on the Cortex-M7, which has no
 * 64-bit exclusive access (the engine's Atomics on BigInt64Array, the load
 * meter's 64-bit counters). One core: each operation runs with every
 * interrupt masked, which makes it atomic against tasks and interrupts. The
 * memory order arguments need nothing more on one core.
 *
 * The functions keep GCC's names through asm labels: defined under those
 * names directly, they would clash with the compiler's builtins. */
#include <stdbool.h>
#include <stdint.h>

#include "fsl_device_registers.h"

/**
 * Masks every interrupt; returns the previous mask.
 */
static inline uint32_t enterCritical(void) {
    uint32_t previous = __get_PRIMASK();
    __disable_irq();
    return previous;
}

/**
 * Restores the mask of enterCritical.
 */
static inline void exitCritical(uint32_t previous) {
    __set_PRIMASK(previous);
}

/* GCC's names given to the functions below (asm labels) */
uint64_t atomicLoad8(const volatile void *address, int order) __asm__("__atomic_load_8");
void atomicStore8(volatile void *address, uint64_t value, int order) __asm__("__atomic_store_8");
uint64_t atomicExchange8(volatile void *address, uint64_t value, int order) __asm__("__atomic_exchange_8");
bool atomicCompareExchange8(volatile void *address, void *expected, uint64_t desired, bool weak, int success,
                            int failure) __asm__("__atomic_compare_exchange_8");
uint64_t atomicFetchAdd8(volatile void *address, uint64_t value, int order) __asm__("__atomic_fetch_add_8");
uint64_t atomicFetchSub8(volatile void *address, uint64_t value, int order) __asm__("__atomic_fetch_sub_8");
uint64_t atomicFetchAnd8(volatile void *address, uint64_t value, int order) __asm__("__atomic_fetch_and_8");
uint64_t atomicFetchOr8(volatile void *address, uint64_t value, int order) __asm__("__atomic_fetch_or_8");
uint64_t atomicFetchXor8(volatile void *address, uint64_t value, int order) __asm__("__atomic_fetch_xor_8");

/**
 * Reads the 64-bit value.
 */
uint64_t atomicLoad8(const volatile void *address, int order) {
    (void)order;
    uint32_t mask = enterCritical();
    uint64_t value = *(const volatile uint64_t *)address;
    exitCritical(mask);
    return value;
}

/**
 * Writes the 64-bit value.
 */
void atomicStore8(volatile void *address, uint64_t value, int order) {
    (void)order;
    uint32_t mask = enterCritical();
    *(volatile uint64_t *)address = value;
    exitCritical(mask);
}

/**
 * Writes 'value', returns the previous one.
 */
uint64_t atomicExchange8(volatile void *address, uint64_t value, int order) {
    (void)order;
    uint32_t mask = enterCritical();
    uint64_t previous = *(volatile uint64_t *)address;
    *(volatile uint64_t *)address = value;
    exitCritical(mask);
    return previous;
}

/**
 * Writes 'desired' if the value is *expected (true); else puts the value
 * into *expected (false).
 */
bool atomicCompareExchange8(volatile void *address, void *expected, uint64_t desired, bool weak, int success,
                            int failure) {
    (void)weak;
    (void)success;
    (void)failure;
    uint32_t mask = enterCritical();
    uint64_t current = *(volatile uint64_t *)address;
    bool equal = current == *(uint64_t *)expected;
    if (equal) {
        *(volatile uint64_t *)address = desired;
    } else {
        *(uint64_t *)expected = current;
    }

    exitCritical(mask);
    return equal;
}

/* The read-modify-write operations: the previous value returned. */
#define FETCH_OPERATION(name, operator)                                         \
    uint64_t name(volatile void *address, uint64_t value, int order) {          \
        (void)order;                                                            \
        uint32_t mask = enterCritical();                                        \
        uint64_t previous = *(volatile uint64_t *)address;                      \
        *(volatile uint64_t *)address = previous operator value;                \
        exitCritical(mask);                                                     \
        return previous;                                                        \
    }

FETCH_OPERATION(atomicFetchAdd8, +)
FETCH_OPERATION(atomicFetchSub8, -)
FETCH_OPERATION(atomicFetchAnd8, &)
FETCH_OPERATION(atomicFetchOr8, |)
FETCH_OPERATION(atomicFetchXor8, ^)
