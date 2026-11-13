/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A file the page gives the user to save (the browser asks where, or puts it
// in its downloads folder). x4js has nothing for it: the browser's own way,
// a Blob and a link clicked, done here once.

/**
 * Offers the text 'parts' (joined as they are) as the file 'name' to save.
 */
export function downloadText(name: string, parts: string[]) {
    // the text as a file, behind a temporary URL
    const blob = new Blob(parts, { type: "text/plain" });
    const url = URL.createObjectURL(blob);

    // a link to it, clicked (never added to the page), then the URL let go
    const link = document.createElement("a");
    link.href = url;
    link.download = name;
    link.click();
    URL.revokeObjectURL(url);
}
