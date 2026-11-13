/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A log of the page (traffic of a UART, transfers of an I2C bus): a box of
// lines that scrolls by itself. It follows its newest line while the user
// leaves it at the bottom; scrolled up to read older lines, it stays where
// the user put it, until the user scrolls back to the bottom.
//
// Only the log scrolls: the element's scrollTop is set directly, because
// scrollIntoView also scrolls every box around it (the column of the
// sections jumped to the log at each new line). x4 has no call for the
// scroll position of a component, hence the element.
//
// Why a flag rather than a look at the position before each new line: a
// folded section hides its body (display: none), which loses the scroll
// position; unfolded, the log is back at its top while still following.
import { BoxProps, VBox } from "x4js";

// How far from the bottom (pixels) the log still counts as at its bottom:
// the scroll position of a browser is not always a whole pixel.
const BOTTOM_MARGIN = 4;

export class LogView extends VBox {
    static classPrefix = "twin";

    private _follow = true;     // the newest line kept in view (the user left the log at its bottom)

    /**
     * An empty log, following its newest line.
     */
    constructor(props: BoxProps) {
        super(props);

        // the user scrolls: following again only once back at the bottom
        this.addDOMEvent("scroll", () => this._onScroll());
    }

    /**
     * Shows the newest line, unless the user scrolled up to read older ones;
     * to call after adding lines.
     */
    followNewest() {
        if (this._follow) {
            this.dom.scrollTop = this.dom.scrollHeight;
        }
    }

    /**
     * Empties the log, which follows its newest line again (clearContent
     * alone keeps the choice of the user: setContent goes through it).
     */
    clearLines() {
        this.clearContent();
        this._follow = true;
    }

    /**
     * The log scrolled (by the user, or by followNewest): it follows its
     * newest line when its last line is in view.
     */
    private _onScroll() {
        const below = this.dom.scrollHeight - this.dom.scrollTop - this.dom.clientHeight;
        this._follow = below <= BOTTOM_MARGIN;
    }
}
