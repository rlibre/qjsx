/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The view of a touch part of qjsx (host/twin/parts/ft6336, gt911): one
// line, where the finger is. The model publishes { screen, pressed, x, y }
// (the finger in pixels of the glass); the screen it covers is recorded
// (touchlink.ts), so that the mouse on that screen touches it. The board
// view does not show it (user: the touch panel is part of its screen): it is
// made for that link.
import { BoxProps, Label, VBox } from "x4js";
import type { PartHandle } from "./partview";
import { tx } from "./texts";
import { coverScreen } from "./touchlink";

// What the model publishes.
interface TouchState {
    screen:  string;    // the id of the screen part it covers
    pressed: boolean;   // a finger on it
    x:       number;    // the finger, pixels of the glass
    y:       number;
}

export class TouchView extends VBox {
    private _line: Label;           // where the finger is

    /**
     * The line, then each state shown; the screen it covers recorded.
     */
    constructor(props: BoxProps, part: PartHandle) {
        super({ ...props, cls: "touch-view" });
        this._line = new Label({ text: tx.twin.touch_none });
        this.setContent(this._line);

        part.onState(value => {
            const state = value as unknown as TouchState;
            coverScreen(state.screen, part);
            this._line.setText(this._text(state));
        });
    }

    /**
     * "a finger at 120, 45 (pixels of the glass) · over screen", or "not
     * touched · over screen".
     */
    private _text(state: TouchState) {
        const where = state.pressed ? `${tx.twin.touch_at} ${state.x}, ${state.y} (${tx.twin.touch_glass})` : tx.twin.touch_none;
        return `${where} · ${tx.twin.touch_screen} ${state.screen}`;
    }
}
