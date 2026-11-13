/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A warning the page shows itself (an x4js Notification, a few seconds, or
// until it is closed): what it refuses before asking the twin, as listening
// on TCP or wiring a UART without a port.
import { Notification } from "x4js";

// How long a warning stays, in seconds.
const WARNING_SECONDS = 5;

/**
 * Shows 'text' under 'title' as a warning.
 */
export function warn(title: string, text: string) {
    const notification = new Notification({ title, text, mode: "danger", closable: true });
    notification.display(WARNING_SECONDS);
}
