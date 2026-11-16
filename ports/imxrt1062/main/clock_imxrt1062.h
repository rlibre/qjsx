/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/* The clocks of the i.MX RT1062 (clock_imxrt1062.c). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Sets the run clocks (core 600 MHz, IPG 150 MHz, PERCLK 24 MHz); called once
 * by main() before anything else.
 */
void qjsxStartClocks(void);

/**
 * The frequency of the board's crystal, in Hz.
 */
uint32_t qjsxCrystalHz(void);

#ifdef __cplusplus
}
#endif
