/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The project file qjsx.json, at the root of a project:
//   {
//     "name": "demo",                 package name (default: the directory name)
//     "main": "src/main.js",          module run at start
//     "sources": ["src/**/*.js"],     modules compiled into the package
//     "exclude": [],                  patterns removed from 'sources'
//     "output": "build/packages",     where packages are written
//     "keep": 10,                     number of packages kept per project
//     "target": "serial:COM3",        default device (deploy, debug)
//     "debugPort": 9339,              port of the debugger server (qjsx debug)
//     "env": { "LOG": "debug" }       process.env of the program on a device
//                                     (strings only; copied into the manifest)
//     "desktop": {                    the program as a program of the PC (an
//       "rootFolder": "${userAppDataFolder}",    executable, qjsx run): its
//       "permissions": {                         root folder (process.cwd())
//         "read": ["${rootFolder}"],             and what it may reach;
//         "write": ["${rootFolder}"],            nothing without them (a root
//         "net": ["api.example.com:443"],        alone may be read). Each kind:
//         "env": ["HOME"], "serial": ["COM3"]    true or a list. Copied into
//       }                                        the manifest
//     }
//     "libraries": ["libs/sensors.qjslib"]
//                                     libraries the program may import, besides
//                                     the standard one (paths from the project)
//   }
//
// A library project (qjsx mklib) has 'modules' instead of 'main' and
// 'sources', and a version:
//   {
//     "name": "sensors",              library name: its modules are imported
//     "version": "1.0.0",             as "sensors/<module>"
//     "modules": {                    module -> source file
//       "bme280": "src/bme280.js",    import { read } from "sensors/bme280";
//       "filter": "src/filter.js"
//     },
//     "output": "build/packages"      where sensors.qjslib is written
//   }
import * as fs from "sys/fs";
import { join, basename, resolve } from "stdlib/path";

export const PROJECT_FILE = "qjsx.json";

const DEFAULTS = {
    main: "src/main.js",
    sources: ["src/**/*.js"],
    exclude: [],
    output: "build/packages",
    keep: 10,
    target: undefined,
    debugPort: 9339,
    env: undefined,
    desktop: undefined,
    libraries: [],
    version: undefined,
    modules: undefined,
};

const TYPES = {
    name: "string",
    main: "string",
    sources: "array",
    exclude: "array",
    output: "string",
    keep: "number",
    target: "string",
    debugPort: "number",
    env: "object",
    desktop: "object",
    libraries: "array",
    version: "string",
    modules: "object",
    twin: "object",
};

// A name made of letters, digits, '.', '_' and '-': project and library
// names, and each element of a module name ("drivers/bme280").
const NAME_PATTERN = /^[A-Za-z0-9._-]+$/;

function isModuleName(name) {
    return name.split("/").every(element => NAME_PATTERN.test(element) && element !== "." && element !== "..");
}

// The variables a path of "desktop" may start with.
const DESKTOP_VARIABLES = ["executableFolder", "userAppDataFolder", "currentFolder", "rootFolder"];

// The kinds of "desktop.permissions"; read and write hold paths.
const PERMISSION_KINDS = ["read", "write", "net", "env", "serial"];

// Checks a path of "desktop": it starts with a known variable (followed by
// '/' or nothing) or is absolute. 'allowRoot': ${rootFolder} may be used.
function checkDesktopPath(file, where, path, allowRoot) {
    const variable = /^\$\{([^}]*)\}(.*)$/.exec(path);
    if (variable === null) {
        const absolute = path.startsWith("/") || /^[A-Za-z]:[\\/]/.test(path);
        if (!absolute) {
            throw new Error(`${file}: '${where}': "${path}" must start with \${${DESKTOP_VARIABLES.join("}, \${")}} or be absolute`);
        }
        return;
    }
    const [, name, rest] = variable;
    if (!DESKTOP_VARIABLES.includes(name)) {
        throw new Error(`${file}: '${where}': unknown variable \${${name}} (known: \${${DESKTOP_VARIABLES.join("}, \${")}})`);
    }
    if (rest !== "" && !rest.startsWith("/")) {
        throw new Error(`${file}: '${where}': "${path}": a variable is followed by '/' or ends the path`);
    }
    if (name === "rootFolder" && !allowRoot) {
        throw new Error(`${file}: '${where}': \${rootFolder} needs 'desktop.rootFolder'`);
    }
}

// Checks the "desktop" section (see the top of the file).
// The "twin" section (the digital twin of the project, "qjsx agent"): its
// shape only; the agent checks the board and each part (their models).
function checkTwin(file, twin) {
    if (typeof twin.board !== "string" || twin.board === "") {
        throw new Error(`${file}: 'twin.board' (the board of the twin) must be a name or a .json path`);
    }
    if (twin.parts !== undefined && !Array.isArray(twin.parts)) {
        throw new Error(`${file}: 'twin.parts' must be an array of parts`);
    }
    for (const [name, port] of Object.entries(twin.ports ?? {})) {
        const known = ["agent", "ui", "usb", "uart"].includes(name);
        if (!known || !Number.isInteger(port) || port < 1 || port > 65535) {
            throw new Error(`${file}: 'twin.ports' holds agent, ui, usb, uart: ports from 1 to 65535`);
        }
    }
}

function checkDesktop(file, desktop) {
    for (const key of Object.keys(desktop)) {
        if (key !== "rootFolder" && key !== "permissions") {
            throw new Error(`${file}: unknown setting 'desktop.${key}'`);
        }
    }
    const { rootFolder, permissions } = desktop;
    if (rootFolder !== undefined) {
        if (typeof rootFolder !== "string") {
            throw new Error(`${file}: 'desktop.rootFolder' must be a string`);
        }
        checkDesktopPath(file, "desktop.rootFolder", rootFolder, false);
    }
    if (permissions === undefined) {
        return;
    }
    if (typeOf(permissions) !== "object") {
        throw new Error(`${file}: 'desktop.permissions' must be an object`);
    }
    for (const [kind, value] of Object.entries(permissions)) {
        const where = `desktop.permissions.${kind}`;
        if (!PERMISSION_KINDS.includes(kind)) {
            throw new Error(`${file}: unknown permission '${where}' (known: ${PERMISSION_KINDS.join(", ")})`);
        }
        if (typeof value === "boolean") {
            continue;
        }
        if (!Array.isArray(value) || value.some(item => typeof item !== "string")) {
            throw new Error(`${file}: '${where}' must be true, false or a list of strings`);
        }
        if (kind === "read" || kind === "write") {
            for (const path of value) {
                checkDesktopPath(file, where, path, rootFolder !== undefined);
            }
        }
    }
}

function typeOf(value) {
    if (value === null) {
        return "null";
    }
    return Array.isArray(value) ? "array" : typeof value;
}

export function loadProject(directory) {
    const file = join(directory, PROJECT_FILE);
    if (!fs.existsSync(file)) {
        throw new Error(`no ${PROJECT_FILE} in ${resolve(directory)}`);
    }
    let settings;
    try {
        settings = JSON.parse(fs.readFileSync(file, "utf8"));
    } catch (error) {
        throw new Error(`${file}: ${error.message}`);
    }
    for (const [key, value] of Object.entries(settings)) {
        if (!(key in TYPES)) {
            throw new Error(`${file}: unknown setting '${key}'`);
        }
        if (typeOf(value) !== TYPES[key]) {
            throw new Error(`${file}: '${key}' must be a ${TYPES[key]}`);
        }
    }
    for (const [key, value] of Object.entries(settings.env ?? {})) {
        if (typeof value !== "string") {
            throw new Error(`${file}: 'env.${key}' must be a string`);
        }
    }
    if (settings.desktop !== undefined) {
        checkDesktop(file, settings.desktop);
    }
    if (settings.twin !== undefined) {
        checkTwin(file, settings.twin);
    }
    for (const library of settings.libraries ?? []) {
        if (typeof library !== "string") {
            throw new Error(`${file}: 'libraries' must be an array of paths`);
        }
    }
    for (const [module, source] of Object.entries(settings.modules ?? {})) {
        if (!isModuleName(module)) {
            throw new Error(`${file}: module name '${module}' may only contain letters, digits, '.', '_', '-' and '/'`);
        }
        if (typeof source !== "string") {
            throw new Error(`${file}: 'modules.${module}' must be the path of its source file`);
        }
    }
    const name = settings.name ?? basename(resolve(directory));
    if (!NAME_PATTERN.test(name)) {
        throw new Error(`${file}: 'name' may only contain letters, digits, '.', '_' and '-'`);
    }
    return { ...DEFAULTS, ...settings, name, directory };
}
