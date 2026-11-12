/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The HTTP requests of the page to the twin's server (host/twin/js/server.js).
// ServerModel owns the only fetch() of the page (time limit, network failure);
// the views never call it. The live state travels on the WebSocket instead
// (messages.ts).

// Time a request may take before it counts as failed, in ms.
const REQUEST_TIMEOUT_MS = 1500;

class ServerModel {
    /**
     * GET 'path' on the server of the page: the response, or null when the
     * server does not answer within REQUEST_TIMEOUT_MS (gone, network down).
     */
    protected async get(path: string): Promise<Response | null> {
        const controller = new AbortController();
        const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
        try {
            return await fetch(path, { signal: controller.signal });
        } catch {
            return null;
        } finally {
            clearTimeout(timer);
        }
    }
}

class TwinModel extends ServerModel {
    /**
     * True when the twin answers (its board description can be read).
     */
    async isReachable(): Promise<boolean> {
        const answer = await this.get("/board.json");
        return answer !== null && answer.ok;
    }
}

export const twinModel = new TwinModel();
