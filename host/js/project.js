// The project file espjs.json, at the root of a project:
//   {
//     "name": "demo",                 package name (default: the directory name)
//     "main": "src/main.js",          module run at start
//     "sources": ["src/**/*.js"],     modules compiled into the package
//     "exclude": [],                  patterns removed from 'sources'
//     "output": "build/packages",     where packages are written
//     "keep": 10,                     number of packages kept per project
//     "target": "serial:COM3",        default device (deploy, debug)
//     "env": { "LOG": "debug" }       process.env of the program on a device
//                                     (strings only; copied into the manifest)
//     "libraries": ["libs/sensors.qjslib"]
//                                     libraries the program may import, besides
//                                     the standard one (paths from the project)
//   }
//
// A library project (espjs mklib) has 'modules' instead of 'main' and
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
import * as fs from "fs";
import { join, basename, resolve } from "stdlib/path";

export const PROJECT_FILE = "espjs.json";

const DEFAULTS = {
    main: "src/main.js",
    sources: ["src/**/*.js"],
    exclude: [],
    output: "build/packages",
    keep: 10,
    target: undefined,
    env: undefined,
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
    env: "object",
    libraries: "array",
    version: "string",
    modules: "object",
};

// A name made of letters, digits, '.', '_' and '-': project and library
// names, and each element of a module name ("drivers/bme280").
const NAME_PATTERN = /^[A-Za-z0-9._-]+$/;

function isModuleName(name) {
    return name.split("/").every(element => NAME_PATTERN.test(element) && element !== "." && element !== "..");
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
