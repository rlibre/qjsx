/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The TypeScript declarations of the JS API (a .d.ts next to each module of
// the repository, embedded as qjsx:types) written in a project, for VS Code.
//
// They go to <project>/.qjsx/types/index.d.ts, a folder of qjsx in the
// project (not node_modules/@types: that is npm's DefinitelyTyped name space,
// and "npm install" removes from node_modules what package.json does not
// list). TypeScript (and so VS Code) loads them whatever the "include" of the
// project's configuration when compilerOptions.types names the folder:
// "types": ["./.qjsx/types"] (TypeScript 6 and later load no types package by
// themselves). Without any jsconfig.json / tsconfig.json, one is written
// with it (ES library only: no DOM names such as window or fetch, which qjsx
// does not have). An existing one is never changed: a warning says what to
// add when its types do not name the folder.
import * as fs from "sys/fs";
import { join } from "stdlib/path";
import { TYPINGS } from "qjsx:types";

const TYPES_FOLDER = ".qjsx/types";           // in the project
const TYPES_REFERENCE = "./.qjsx/types";      // its entry in compilerOptions.types
const CONFIG_FILES = ["jsconfig.json", "tsconfig.json"];

// The index.d.ts of the folder: every declaration file, globals first.
function typingsText() {
    const parts = ["// Written by \"qjsx build\" from the typings of qjsx: do not edit.\n"];
    const ordered = [...TYPINGS].sort((a, b) => {
        const aGlobals = a.name === "globals" ? 0 : 1;
        const bGlobals = b.name === "globals" ? 0 : 1;
        return aGlobals - bGlobals || (a.name < b.name ? -1 : a.name > b.name ? 1 : 0);
    });
    for (const typing of ordered) {
        parts.push(`// ### ${typing.name} ###\n`);
        parts.push(typing.text);
    }
    return parts.join("\n");
}

// Writes 'text' to 'file' unless it holds it already (VS Code reloads a
// changed declaration file).
function writeIfChanged(file, text) {
    let current = null;
    try {
        current = fs.readFileSync(file, "utf8");
    } catch {
        // missing: written below
    }
    if (current !== text) {
        fs.writeFileSync(file, text);
    }
}

// The JSON of a jsconfig.json / tsconfig.json, which may hold comments and
// trailing commas (JSONC). Comments outside strings are removed, then commas
// before '}' or ']'.
export function parseJsonc(text) {
    let json = "";
    let index = 0;
    while (index < text.length) {
        const character = text[index];
        const next = text[index + 1];

        // a string: copied whole, escapes included
        if (character === "\"") {
            let end = index + 1;
            while (end < text.length && text[end] !== "\"") {
                end += text[end] === "\\" ? 2 : 1;
            }
            json += text.slice(index, end + 1);
            index = end + 1;
            continue;
        }

        // a line comment, then a block comment: dropped
        if (character === "/" && next === "/") {
            const end = text.indexOf("\n", index);
            index = end < 0 ? text.length : end;
            continue;
        }
        if (character === "/" && next === "*") {
            const end = text.indexOf("*/", index + 2);
            index = end < 0 ? text.length : end + 2;
            continue;
        }

        json += character;
        index++;
    }

    // trailing commas
    const withoutCommas = json.replace(/,(\s*[}\]])/g, "$1");
    return JSON.parse(withoutCommas);
}

// The jsconfig.json written when the project has no configuration.
function defaultConfig(output) {
    const config = {
        compilerOptions: {
            target: "esnext",
            module: "esnext",
            moduleResolution: "bundler",
            lib: ["esnext"],
            types: [TYPES_REFERENCE],
        },
        exclude: [".qjsx", output],
    };
    return JSON.stringify(config, null, 4) + "\n";
}

// True when the entry 'entry' of compilerOptions.types names the typings
// folder (".qjsx/types", "./.qjsx/types/", ".\\.qjsx\\types").
function namesTypesFolder(entry) {
    const normalized = String(entry).replace(/\\/g, "/").replace(/^(\.\/)+/, "").replace(/\/+$/, "");
    return normalized === TYPES_FOLDER;
}

// What to add to the configuration 'config' (parsed, from 'file') so that
// VS Code sees the typings: { warning, fix } (a sentence, and the JSON text
// to add, to copy as it is), or null when nothing is missing.
function missingSetting(config, file) {
    const options = config.compilerOptions ?? {};
    if (!Array.isArray(options.types)) {
        return { warning: `add to compilerOptions in ${file}:`, fix: `"types": ["${TYPES_REFERENCE}"]` };
    }
    if (!options.types.some(namesTypesFolder)) {
        return { warning: `add to compilerOptions.types in ${file}:`, fix: `"${TYPES_REFERENCE}"` };
    }
    return null;
}

// Writes the typings in the project in 'directory' (its output folder is
// 'output', relative). Returns { file, configWritten, warning, fix }:
//   - file: the index.d.ts written;
//   - configWritten: the jsconfig.json written, or null when the project
//     had a configuration;
//   - warning: a sentence when that configuration hides the typings (also
//     when it cannot be read: VS Code would not read it either), or null;
//   - fix: the JSON text to add to it, or null.
export function writeTypings(directory, output) {
    // the declarations, in .qjsx/types
    const folder = join(directory, TYPES_FOLDER);
    fs.mkdirSync(folder, { recursive: true });
    const file = join(folder, "index.d.ts");
    writeIfChanged(file, typingsText());

    // the configuration of the project: read it, or write one
    const existing = CONFIG_FILES.map(name => join(directory, name)).find(path => fs.existsSync(path));
    if (existing === undefined) {
        const configFile = join(directory, CONFIG_FILES[0]);
        fs.writeFileSync(configFile, defaultConfig(output));
        return { file, configWritten: configFile, warning: null, fix: null };
    }

    // an existing one must name the folder in its types
    let config;
    try {
        config = parseJsonc(fs.readFileSync(existing, "utf8"));
    } catch (error) {
        return { file, configWritten: null, warning: `${existing} cannot be read (${error.message}).`, fix: null };
    }
    const missing = missingSetting(config, existing);
    if (missing === null) {
        return { file, configWritten: null, warning: null, fix: null };
    }
    return { file, configWritten: null, warning: missing.warning, fix: missing.fix };
}
