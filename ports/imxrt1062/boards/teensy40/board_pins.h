/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The digital pins of the Teensy 4.0, numbered as printed on the board (0 to
// 39; the LED is 13): for each one its pad of the RT1062 and its bit in the
// fast GPIO port mirroring it (GPIO6 to GPIO9 mirror GPIO1 to GPIO4). From
// PJRC's teensy4/core_pins.h (CORE_PINn_PORTREG, _BIT, _CONFIG), checked
// 2026-11-16. Used by hal_gpio_imxrt1062.cpp.
#pragma once

#include <stdint.h>

#include "fsl_device_registers.h"

namespace board {

// One pin: its fast GPIO port (6 to 9), its bit there, its pad's mux and
// control registers in the IOMUXC.
struct PinMap {
    uint8_t                 port;      // 6, 7, 8 or 9
    uint8_t                 bit;       // 0 to 31
    iomuxc_sw_mux_ctl_pad_t mux;       // index of SW_MUX_CTL_PAD
    iomuxc_sw_pad_ctl_pad_t control;   // index of SW_PAD_CTL_PAD
};

// a pin from its port, bit and pad name
#define QJSX_PIN(port, bit, pad) { port, bit, kIOMUXC_SW_MUX_CTL_PAD_##pad, kIOMUXC_SW_PAD_CTL_PAD_##pad }

const PinMap PINS[] = {
    QJSX_PIN(6, 3, GPIO_AD_B0_03),    // 0
    QJSX_PIN(6, 2, GPIO_AD_B0_02),    // 1
    QJSX_PIN(9, 4, GPIO_EMC_04),      // 2
    QJSX_PIN(9, 5, GPIO_EMC_05),      // 3
    QJSX_PIN(9, 6, GPIO_EMC_06),      // 4
    QJSX_PIN(9, 8, GPIO_EMC_08),      // 5
    QJSX_PIN(7, 10, GPIO_B0_10),      // 6
    QJSX_PIN(7, 17, GPIO_B1_01),      // 7
    QJSX_PIN(7, 16, GPIO_B1_00),      // 8
    QJSX_PIN(7, 11, GPIO_B0_11),      // 9
    QJSX_PIN(7, 0, GPIO_B0_00),       // 10
    QJSX_PIN(7, 2, GPIO_B0_02),       // 11
    QJSX_PIN(7, 1, GPIO_B0_01),       // 12
    QJSX_PIN(7, 3, GPIO_B0_03),       // 13: the LED
    QJSX_PIN(6, 18, GPIO_AD_B1_02),   // 14
    QJSX_PIN(6, 19, GPIO_AD_B1_03),   // 15
    QJSX_PIN(6, 23, GPIO_AD_B1_07),   // 16
    QJSX_PIN(6, 22, GPIO_AD_B1_06),   // 17
    QJSX_PIN(6, 17, GPIO_AD_B1_01),   // 18
    QJSX_PIN(6, 16, GPIO_AD_B1_00),   // 19
    QJSX_PIN(6, 26, GPIO_AD_B1_10),   // 20
    QJSX_PIN(6, 27, GPIO_AD_B1_11),   // 21
    QJSX_PIN(6, 24, GPIO_AD_B1_08),   // 22
    QJSX_PIN(6, 25, GPIO_AD_B1_09),   // 23
    QJSX_PIN(6, 12, GPIO_AD_B0_12),   // 24
    QJSX_PIN(6, 13, GPIO_AD_B0_13),   // 25
    QJSX_PIN(6, 30, GPIO_AD_B1_14),   // 26
    QJSX_PIN(6, 31, GPIO_AD_B1_15),   // 27
    QJSX_PIN(8, 18, GPIO_EMC_32),     // 28
    QJSX_PIN(9, 31, GPIO_EMC_31),     // 29
    QJSX_PIN(8, 23, GPIO_EMC_37),     // 30
    QJSX_PIN(8, 22, GPIO_EMC_36),     // 31
    QJSX_PIN(7, 12, GPIO_B0_12),      // 32
    QJSX_PIN(9, 7, GPIO_EMC_07),      // 33
    QJSX_PIN(8, 15, GPIO_SD_B0_03),   // 34
    QJSX_PIN(8, 14, GPIO_SD_B0_02),   // 35
    QJSX_PIN(8, 13, GPIO_SD_B0_01),   // 36
    QJSX_PIN(8, 12, GPIO_SD_B0_00),   // 37
    QJSX_PIN(8, 17, GPIO_SD_B0_05),   // 38
    QJSX_PIN(8, 16, GPIO_SD_B0_04),   // 39
};

#undef QJSX_PIN

const int PIN_COUNT = (int)(sizeof(PINS) / sizeof(PINS[0]));

} // namespace board
