/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The header of the page: the logo and the title, the board, the state of
// the link to the twin, and the button of the settings.
import { Button, Flex, HBox, Image, Label } from "x4js";
import { tx } from "./texts";

import gearIcon from "./assets/gear.svg";
import logo from "./assets/logo.svg";

export interface AppHeaderProps {
    settings: () => void;    // the settings button was clicked
}

export class AppHeader extends HBox {
    declare protected refs: { board: Label; status: Label };

    /**
     * The header, "connecting..." until setBoard().
     */
    constructor(props: AppHeaderProps) {
        super({ cls: "app-header" });

        this.setContent([
            new Image({ cls: "logo", src: logo, alt: "qjsx" }),
            new Label({ cls: "title", text: tx.twin.title }),
            this.refs.board = new Label({ cls: "board-of", text: "" }),
            new Flex(),
            this.refs.status = new Label({ cls: "status", text: tx.twin.connecting }),
            new Button({ cls: "settings outline", icon: gearIcon, tooltip: tx.twin.settings, click: () => props.settings() }),
        ]);
    }

    /**
     * Shows the board the twin plays; the link is up.
     */
    setBoard(name: string, chip: string) {
        this.refs.board.setText(`${name} (${chip})`);
        this.refs.status.setText(tx.twin.connected);
        this.refs.status.removeClass("lost");
    }

    /**
     * The link to the twin is lost.
     */
    showDisconnected() {
        this.refs.status.setText(tx.twin.disconnected);
        this.refs.status.addClass("lost");
    }
}
