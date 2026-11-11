/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// File matching with glob patterns relative to a root directory:
//   "src/**/*.js"   any .js file under src/
//   "*"             any characters except '/'
//   "?"             one character except '/'
//   "**/"           any number of directories (including none)
import * as fs from "fs";
import { join } from "stdlib/path";

const SKIPPED_DIRECTORIES = new Set([".git"]);

export function globToRegExp(pattern) {
    let expression = "";
    for (let i = 0; i < pattern.length; i++) {
        const c = pattern[i];
        if (c === "*" && pattern[i + 1] === "*") {
            const followedBySlash = pattern[i + 2] === "/";
            expression += followedBySlash ? "(?:.*/)?" : ".*";
            i += followedBySlash ? 2 : 1;
        } else if (c === "*") {
            expression += "[^/]*";
        } else if (c === "?") {
            expression += "[^/]";
        } else {
            expression += c.replace(/[.+^${}()|[\]\\]/g, "\\$&");
        }
    }
    return new RegExp(`^${expression}$`);
}

// Every file under 'root', as paths relative to it with '/' separators.
export function listFiles(root, relative = "") {
    const directory = relative.length > 0 ? join(root, relative) : root;
    const files = [];
    for (const name of fs.readdirSync(directory)) {
        const path = relative.length > 0 ? `${relative}/${name}` : name;
        if (fs.statSync(join(root, path)).isDirectory()) {
            if (!SKIPPED_DIRECTORIES.has(name)) {
                files.push(...listFiles(root, path));
            }
        } else {
            files.push(path);
        }
    }
    return files;
}

// Files matching one of 'include' and none of 'exclude', sorted.
export function matchFiles(root, include, exclude = []) {
    const included = include.map(globToRegExp);
    const excluded = exclude.map(globToRegExp);
    return listFiles(root)
        .filter(path => included.some(pattern => pattern.test(path)))
        .filter(path => !excluded.some(pattern => pattern.test(path)))
        .sort();
}
