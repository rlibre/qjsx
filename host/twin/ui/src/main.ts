/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The web page of the digital twin ("qjsx agent --board <name>"), served by
// the twin itself (host/twin/js/server.js). Built by "npm run build" into
// dist/, which the qjsx build embeds.
import { Application, initTooltips, selectLanguage } from "x4js";
import { twinModel } from "./model";
import { addTexts } from "./texts";
import { TwinView } from "./twinview";
import "./main.scss";

// How often a lost page checks whether the twin is back, in ms.
const RETRY_MS = 2000;

class TwinApp extends Application {
    private _view: TwinView;   // the main view

    /**
     * Builds the page and opens the link to the twin.
     */
    constructor() {
        super();

        // the view (it applies the theme of the settings), then the link:
        // the global messages travel on /ws
        this._view = new TwinView({ flex: true });
        this.setMainView(this._view);
        this.setupSocketMessaging(undefined, () => this._onLost());
    }

    /**
     * The link is lost: shows it, then reloads the page once the twin
     * answers again.
     */
    private _onLost() {
        this._view.showDisconnected();
        this.setInterval("retry", RETRY_MS, async () => {
            if (await twinModel.isReachable()) {
                window.location.reload();
            }
        });
    }
}

// the texts, in English, before any component is built; the tooltips (prop
// "tooltip") are shown once initTooltips() is called
addTexts();
selectLanguage("en");
initTooltips();
new TwinApp();
