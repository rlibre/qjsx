/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/* The clocks of the i.MX RT1062 at run speed (the SDK's board files, which do
 * this for the EVK, are not used: they are in its examples repository):
 *   core (ARM PLL 1200 MHz / 2)  600 MHz, the core voltage raised first
 *   AHB                          600 MHz
 *   IPG (peripherals)            150 MHz
 *   PERCLK (PIT, GPT)            24 MHz, from the crystal
 * The FlexSPI clock (the flash) is left as the boot ROM set it from the
 * configuration block; the USB PLL is started by the USB link. */
#include "clock_imxrt1062.h"

#include "fsl_clock.h"
#include "fsl_device_registers.h"

/* the crystal of the board */
#define XTAL_HZ 24000000u

/* DCDC target of the core voltage: 0x13 = 1.275 V, needed at 600 MHz */
#define CORE_VOLTAGE_TARGET 0x13u

/* ARM PLL: 24 MHz * 100 / 2 = 1200 MHz */
#define ARM_PLL_LOOP_DIVIDER 100u

/* register values of the muxes (reference manual, CCM) */
#define PERIPH_CLK2_FROM_OSC 1u      /* periph_clk2 = the 24 MHz oscillator */
#define PERIPH_FROM_CLK2 1u          /* periph_clk = periph_clk2 */
#define PERIPH_FROM_PRE_PERIPH 0u    /* periph_clk = pre_periph_clk */
#define PRE_PERIPH_FROM_ARM_PLL 3u   /* pre_periph_clk = the ARM PLL (divided by ARM_PODF) */
#define PERCLK_FROM_OSC 1u           /* perclk = the 24 MHz oscillator */

/* register values of the dividers: the division minus one */
#define DIVIDE_BY_1 0u
#define DIVIDE_BY_2 1u
#define DIVIDE_BY_4 3u

/**
 * 1. the crystal as the reference, its frequency for the drivers;
 * 2. the core voltage up (the core must not run at 600 MHz at the boot
 *    voltage);
 * 3. the core on the 24 MHz oscillator while the ARM PLL changes;
 * 4. the ARM PLL at 1200 MHz, the dividers, then the core back on it;
 * 5. PERCLK from the oscillator (the PIT counts at 24 MHz);
 * 6. SystemCoreClock for FreeRTOS's tick.
 */
void qjsxStartClocks(void) {
    // the crystal
    CLOCK_InitExternalClk(false);
    CLOCK_SwitchOsc(kCLOCK_XtalOsc);
    CLOCK_SetXtalFreq(XTAL_HZ);

    // the core voltage, then wait for the DCDC to settle
    DCDC->REG3 = (DCDC->REG3 & ~DCDC_REG3_TRG_MASK) | DCDC_REG3_TRG(CORE_VOLTAGE_TARGET);
    while ((DCDC->REG0 & DCDC_REG0_STS_DC_OK_MASK) == 0u) {
    }

    // the core on the oscillator meanwhile
    CLOCK_SetDiv(kCLOCK_PeriphClk2Div, DIVIDE_BY_1);
    CLOCK_SetMux(kCLOCK_PeriphClk2Mux, PERIPH_CLK2_FROM_OSC);
    CLOCK_SetMux(kCLOCK_PeriphMux, PERIPH_FROM_CLK2);

    // the ARM PLL and the dividers, then the core on it
    const clock_arm_pll_config_t armPll = { .loopDivider = ARM_PLL_LOOP_DIVIDER, .src = 0 };
    CLOCK_InitArmPll(&armPll);
    CLOCK_SetDiv(kCLOCK_ArmDiv, DIVIDE_BY_2);
    CLOCK_SetDiv(kCLOCK_AhbDiv, DIVIDE_BY_1);
    CLOCK_SetDiv(kCLOCK_IpgDiv, DIVIDE_BY_4);
    CLOCK_SetMux(kCLOCK_PrePeriphMux, PRE_PERIPH_FROM_ARM_PLL);
    CLOCK_SetMux(kCLOCK_PeriphMux, PERIPH_FROM_PRE_PERIPH);

    // PERCLK
    CLOCK_SetMux(kCLOCK_PerclkMux, PERCLK_FROM_OSC);
    CLOCK_SetDiv(kCLOCK_PerclkDiv, DIVIDE_BY_1);

    // what the core runs at now
    SystemCoreClockUpdate();
}

/**
 * The crystal's frequency (for the USB PHY).
 */
uint32_t qjsxCrystalHz(void) {
    return XTAL_HZ;
}
