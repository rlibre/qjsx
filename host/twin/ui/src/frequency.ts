/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Frequencies and shares as short texts, for the page (the I2C buses, the
// PWM outputs).

/**
 * A frequency with three significant digits at most, in the unit that
 * fits: "50 Hz", "1 kHz", "1.22 kHz", "100 kHz", "3.4 MHz".
 */
export function frequencyText(hz: number) {
    const rounded = Number(hz.toPrecision(3));
    if (rounded >= 1000000) {
        return `${rounded / 1000000} MHz`;
    }
    if (rounded >= 1000) {
        return `${rounded / 1000} kHz`;
    }
    return `${rounded} Hz`;
}

/**
 * A share (0 to 1) as a percentage, three significant digits at most:
 * "25 %", "0.1 %", "99.9 %".
 */
export function percentText(share: number) {
    return `${Number((share * 100).toPrecision(3))} %`;
}
