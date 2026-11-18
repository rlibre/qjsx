/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The touch panels laid over the screens of the page. A touch part (the
// models ft6336, gt911) publishes the id of the screen part it covers; its
// view records it here (coverScreen), and the view of that screen sends the
// presses of the mouse to it (touchScreen). One page, one thread: no lock.
import type { PartHandle } from "./partview";

// The touch parts over each screen, by the id of the screen part.
const panels = new Map<string, Set<PartHandle>>();

/**
 * 'part' covers the screen part 'screen' (and no other one any more).
 */
export function coverScreen(screen: string, part: PartHandle) {
    // out of any other screen
    for (const parts of panels.values()) {
        parts.delete(part);
    }

    // over this one
    if (!panels.has(screen)) {
        panels.set(screen, new Set());
    }

    panels.get(screen).add(part);
}

/**
 * Whether a touch part covers the screen part 'screen'.
 */
export function isTouchable(screen: string) {
    return (panels.get(screen)?.size ?? 0) > 0;
}

/**
 * Sends 'action' ("press", "move", "release") with 'args' to every touch
 * part over the screen part 'screen'.
 */
export function touchScreen(screen: string, action: string, args?: unknown) {
    for (const part of panels.get(screen) ?? []) {
        part.send(action, args);
    }
}
