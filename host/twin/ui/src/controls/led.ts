/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// An LED of the board: a disc lit with a color (off: dark).
import { Component, ComponentProps } from "x4js";

/**
 * The CSS color of an LED as the eye sees it: low values are visible, as on a
 * real LED (a WS2812 at 10/255 is already bright).
 */
function visibleColor(red: number, green: number, blue: number) {
    const scale = (value: number) => Math.round(255 * Math.sqrt(value / 255));
    return `rgb(${scale(red)}, ${scale(green)}, ${scale(blue)})`;
}

export class Led extends Component<ComponentProps> {
    static classPrefix = "twin";

    /**
     * Lights the LED with a color, each channel 0 to 255 (all 0: off). The
     * color is the CSS variable --led-color: the lens, its glow (main.scss).
     */
    setColor(red: number, green: number, blue: number) {
        const off = red === 0 && green === 0 && blue === 0;
        this.setClass("off", off);
        this.setStyleVariable("--led-color", off ? "" : visibleColor(red, green, blue));
    }
}
