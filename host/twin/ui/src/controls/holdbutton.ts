/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A button held down: 'press' when the pointer goes down on it, 'release'
// when it goes up or leaves the button while held (the push buttons of a
// board). An x4 Button has only "click": this one listens to the pointer.
// Plain callbacks: the events of Button cannot be extended by a subclass.
import { Button, ButtonProps } from "x4js";

export interface HoldButtonProps extends ButtonProps {
    press?:   () => void;   // the button goes down
    release?: () => void;   // the button comes back up
}

export class HoldButton extends Button {
    static classPrefix = "twin";

    private _down = false;               // held down now
    private _press: () => void;          // props.press, or nothing
    private _release: () => void;        // props.release, or nothing

    /**
     * A button that reports when it is pressed and released.
     */
    constructor(props: HoldButtonProps) {
        super(props);

        this._press = props.press ?? (() => {});
        this._release = props.release ?? (() => {});

        // the pointer: down presses, up or leaving releases
        this.addDOMEvent("pointerdown", (event: PointerEvent) => this._onDown(event));
        this.addDOMEvent("pointerup", () => this._onUp());
        this.addDOMEvent("pointerleave", () => this._onUp());
    }

    /**
     * The pointer goes down: the button is pressed.
     */
    private _onDown(event: PointerEvent) {
        event.preventDefault();
        this._down = true;
        this.addClass("down");
        this._press();
    }

    /**
     * The pointer goes up or leaves: released, if it was held.
     */
    private _onUp() {
        if (!this._down) {
            return;
        }
        this._down = false;
        this.removeClass("down");
        this._release();
    }
}
