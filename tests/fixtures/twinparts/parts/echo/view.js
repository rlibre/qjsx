/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The view of its own of the echo part, loaded by the page of the twin:
// the number of transfers.
export function createView(x4, part) {
    const label = new x4.Label({ text: "no transfer yet" });
    part.onState(state => label.setText(`${state.transfers} transfer(s)`));
    return label;
}
