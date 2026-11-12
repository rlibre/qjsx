/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A section of the board view that folds: a title line (a chevron, an icon,
// the title, a short summary of the state, kept visible when folded) and a
// body. A click on the title line folds or unfolds it; the browser keeps
// whether it is open (loadPState), open the first time.
import { BoxProps, ComponentContent, HBox, Icon, Label, VBox } from "x4js";

import chevronIcon from "../assets/chevron.svg";

export interface SectionProps extends BoxProps {
    title:   string;             // shown in bold
    persist: string;             // name under which the browser keeps whether it is open
    icon?:   string;             // an svg before the title
}

export class Section extends VBox<SectionProps> {
    static classPrefix = "twin";

    declare protected refs: { title: Label; summary: Label; body: VBox };

    private _state: { open: boolean };   // kept by the browser

    /**
     * The section; its content (props.content) goes in the body.
     */
    constructor(props: SectionProps) {
        super({ ...props, content: undefined });

        // open or folded, as the browser kept it
        this._state = this.loadPState(`qjsx.twin.section.${props.persist}`, { open: true });

        this.setContent([
            new HBox({
                cls: "section-header",
                dom_events: { click: () => this.toggle() },
                content: [
                    new Icon({ cls: "chevron", iconId: chevronIcon }),
                    props.icon ? new Icon({ cls: "section-icon", iconId: props.icon }) : null,
                    this.refs.title = new Label({ cls: "section-title", text: props.title }),
                    this.refs.summary = new Label({ cls: "section-summary", flex: true, text: "" }),
                ],
            }),
            this.refs.body = new VBox({ cls: "section-body", content: props.content }),
        ]);

        this._showState();
    }

    /**
     * The short summary on the title line.
     */
    setSummary(text: string) {
        this.refs.summary.setText(text);
    }

    /**
     * What the body shows.
     */
    setBody(content: ComponentContent) {
        this.refs.body.setContent(content);
    }

    /**
     * Folds an open section, opens a folded one (kept by the browser).
     */
    toggle() {
        this._state.open = !this._state.open;
        this._showState();
    }

    /**
     * Shows the body or not, the chevron turned when folded.
     */
    private _showState() {
        this.setClass("folded", !this._state.open);
        this.refs.body.show(this._state.open);
    }
}
