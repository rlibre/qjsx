/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// qjsx build: compiles the project into a .qjspkg package.
//
// Package content (zip, entries stored):
//   manifest.json          format, name, buildId, created, compiler, bytecode
//                          (version), main, env, libraries[], natives[], scripts[]
//   bytecode/<name>.qbc    bytecode without debug info (what runs on the ESP):
//                          no source text, positions nor variable names; the
//                          module name and the function numbers stay
//   debug/<name>.json      debug sidecar of the script
//   src/<path>             exact copy of the compiled source
//
// The modules of the package:
//   - the modules of the program: every file of 'sources', named by its path
//     in the project ("src/main.js");
//   - the library modules they import ("stdlib/path"), and those these
//     import in turn, each one whole, taken from the .qjslib libraries (the
//     standard one and those of 'libraries', see qjsx:library). A device has
//     no compiler and no JS in its firmware: it finds them in the package
//     like any other module.
// Every import is followed when the package is built, which is why 'import()'
// is refused (qjsx:compile). A name that is neither a module of the program
// nor of a library is a native module: it is listed in manifest.natives and
// must exist in the firmware.
//
// manifest.bytecode is the version of the bytecode of the package: a device
// refuses a package that holds another version than the one its engine
// reads. A library made by another compiler is not refused: it holds its
// sources, so its modules are compiled again (a note says so).
//
// Identifiers (those of a script are in qjsx:compile):
//   package buildId = first 32 hex digits of sha256 over the scripts' ids and the main module
import * as fs from "sys/fs";
import { join, resolve } from "stdlib/path";
import { sha256 } from "sys/crypto";
import { writeZip } from "qjsx:zip";
import { matchFiles } from "qjsx:glob";
import { loadProject, PROJECT_FILE } from "qjsx:project";
import { hex, encodeText } from "qjsx:util";
import { compileModule, compilerId, BYTECODE_VERSION } from "qjsx:compile";
import {
    readLibrary, libraryModule, libraryOf, isNativeModule, bareNameError, madeByThisCompiler, standardLibraryFile,
    STANDARD_LIBRARY, NATIVE_PREFIXES,
} from "qjsx:library";

export const PACKAGE_FORMAT = 1;

// A module of the program, compiled from its file: what goes into the
// package ('script' is its record in the manifest).
function programModule(directory, path) {
    const source = fs.readFileSync(join(directory, path), "utf8");
    const compiled = compileModule(path, source);
    const script = {
        path,
        module: true,
        buildId: compiled.buildId,
        sourceHash: compiled.sourceHash,
        bytecode: `bytecode/${path}.qbc`,
        debug: `debug/${path}.json`,
        source: `src/${path}`,
    };
    return {
        script,
        source,
        bytecode: compiled.bytecode,
        sidecar: compiled.sidecar,
        imports: compiled.imports,
        warnings: compiled.warnings,
    };
}

// A module of a library, as it goes into the package: same shape as
// programModule(); its record says which library it comes from.
function packagedLibraryModule(library, name) {
    const module = libraryModule(library, name);
    const script = {
        path: name,
        module: true,
        library: library.name,
        buildId: module.buildId,
        sourceHash: module.sourceHash,
        bytecode: `bytecode/${name}.qbc`,
        debug: `debug/${name}.json`,
        source: `src/${name}.js`,
    };
    return {
        script,
        source: module.source,
        bytecode: module.bytecode,
        sidecar: module.sidecar,
        imports: module.imports,
        warnings: module.warnings,
    };
}

// The libraries the program may import, Map(name -> library): the standard
// one, when its file is there, then those of 'libraries' in qjsx.json.
function loadLibraries(project) {
    const files = project.libraries.map(path => resolve(project.directory, path));
    const standard = standardLibraryFile();
    if (fs.existsSync(standard)) {
        files.unshift(standard);
    }

    const libraries = new Map();
    for (const file of files) {
        const library = readLibrary(file);
        if (NATIVE_PREFIXES.includes(library.name)) {
            throw new Error(`${file}: a library cannot be named '${library.name}', the prefix of the native modules`);
        }
        const sameName = libraries.get(library.name);
        if (sameName !== undefined) {
            throw new Error(`two libraries are named '${library.name}': ${sameName.file} and ${file}`);
        }
        libraries.set(library.name, library);
    }
    return libraries;
}

// What 'importer' imports, when it is not already in the package: a native
// module (sys/..., device/...), added to 'natives' (nothing is returned), or a
// module of a library, returned to be added. Anything else is an error.
function importedModule(importer, imported, libraries, natives) {
    const where = `${importer.script.path} imports '${imported.specifier}'`;

    // a native module: the firmware (or the tool on the PC) has it, the
    // device says which ones at deploy time
    if (isNativeModule(imported.name)) {
        natives.add(imported.name);
        return undefined;
    }

    const libraryName = libraryOf(imported.name);
    const library = libraries.get(libraryName);

    // a module of a known library
    if (library !== undefined) {
        if (!library.modules.has(imported.name)) {
            throw new Error(`${where}: the library '${library.name}' (${library.file}) has no such module`);
        }
        return packagedLibraryModule(library, imported.name);
    }

    // a relative import can only be a module of the program: not found
    if (imported.specifier.startsWith(".")) {
        throw new Error(`${where}: no module '${imported.name}' among the sources`);
    }

    // "<library>/<module>" of a library that is not there
    if (libraryName === STANDARD_LIBRARY) {
        throw new Error(`${where}: the standard library was not found (${standardLibraryFile()})`);
    }
    if (libraryName !== undefined) {
        throw new Error(`${where}: not a module of the program, and no library '${libraryName}' is declared ('libraries' in ${PROJECT_FILE})`);
    }

    // a name without '/': no such module
    throw bareNameError(where, imported.name);
}

// Removes the oldest packages of the project, keeping 'keep' of them.
function prune(output, name, keep, current) {
    const pattern = new RegExp(`^${name.replace(/[.]/g, "\\.")}-[0-9a-f]{12}\\.qjspkg$`);
    const packages = fs.readdirSync(output)
        .filter(file => pattern.test(file))
        .map(file => ({ file: join(output, file), time: fs.statSync(join(output, file)).mtimeMs }))
        .sort((a, b) => b.time - a.time);
    for (const old of packages.slice(Math.max(keep, 1))) {
        if (old.file !== current) {
            fs.unlinkSync(old.file);
        }
    }
}

// Builds the package of the project in 'directory'. Returns { file, buildId,
// scriptCount, size, warnings, notes }:
//   - 'warnings' has one { file, kind, line, column } per use of eval() or of
//     the Function constructor in a compiled module (qjsx:compile,
//     warningText);
//   - 'notes' are sentences about what the build did besides compiling (a
//     library compiled again because another compiler made it).
export function build(directory) {
    const project = loadProject(directory);
    if (project.modules !== undefined) {
        throw new Error(`${join(directory, PROJECT_FILE)} describes a library ('modules'): use 'qjsx mklib'`);
    }
    const sources = matchFiles(directory, project.sources, project.exclude);
    if (!sources.includes(project.main)) {
        throw new Error(`the main module '${project.main}' is not among the sources (${project.sources.join(", ")})`);
    }
    const libraries = loadLibraries(project);

    // the modules of the program; none may bear the name of a library module
    const packaged = new Map();   // module name -> module, in package order
    for (const path of sources) {
        const library = libraries.get(libraryOf(path));
        if (library !== undefined && library.modules.has(path)) {
            throw new Error(`the source '${path}' has the name of a module of the library '${library.name}' (${library.file})`);
        }
        packaged.set(path, programModule(directory, path));
    }

    // follow the imports: add the library modules the program imports, then
    // those they import, until nothing is missing
    const natives = new Set();
    const toFollow = [...packaged.values()];
    while (toFollow.length > 0) {
        const importer = toFollow.shift();
        for (const imported of importer.imports) {
            if (packaged.has(imported.name) || natives.has(imported.name)) {
                continue;
            }
            const added = importedModule(importer, imported, libraries, natives);
            if (added !== undefined) {
                packaged.set(imported.name, added);
                toFollow.push(added);
            }
        }
    }

    // the entries of the archive; two modules must not give the same entry
    const entries = [];
    const entryNames = new Set();
    const scripts = [];
    for (const module of packaged.values()) {
        scripts.push(module.script);
        const moduleEntries = [
            { name: module.script.bytecode, data: module.bytecode },
            { name: module.script.debug, data: encodeText(JSON.stringify(module.sidecar, null, 1)) },
            { name: module.script.source, data: encodeText(module.source) },
        ];
        for (const entry of moduleEntries) {
            if (entryNames.has(entry.name)) {
                throw new Error(`two modules give the package entry '${entry.name}' (one of them is ${module.script.path})`);
            }
            entryNames.add(entry.name);
            entries.push(entry);
        }
    }

    // the libraries actually used; one made by another compiler was compiled
    // again from its sources: say so
    const usedNames = new Set(scripts.map(script => script.library).filter(name => name !== undefined));
    const usedLibraries = [...usedNames].map(name => ({ name, version: libraries.get(name).version }));
    const notes = [];
    for (const name of usedNames) {
        const library = libraries.get(name);
        if (!madeByThisCompiler(library)) {
            notes.push(`library '${name}' (${library.file}) was made by another compiler (${library.compiler}, `
                + `bytecode version ${library.bytecode}): its modules were compiled again from their sources`);
        }
    }

    // what the compiled modules ask of the compiler at run time
    const warnings = [];
    for (const module of packaged.values()) {
        warnings.push(...module.warnings.map(warning => ({ file: module.script.path, ...warning })));
    }

    // the build id covers what changes the program: scripts, main module,
    // env, what it may reach on the PC
    let identity = scripts.map(script => `${script.path}:${script.buildId}`).join("\n") + `\nmain:${project.main}`;
    if (project.env !== undefined) {
        identity += `\nenv:${JSON.stringify(project.env)}`;
    }
    if (project.desktop !== undefined) {
        identity += `\ndesktop:${JSON.stringify(project.desktop)}`;
    }
    const buildId = hex(sha256(identity)).slice(0, 32);
    const manifest = {
        format: PACKAGE_FORMAT,
        name: project.name,
        buildId,
        created: new Date().toISOString(),
        compiler: compilerId(),
        bytecode: BYTECODE_VERSION,
        main: project.main,
        env: project.env,   // process.env on a device; left out of the JSON when undefined
        desktop: project.desktop,   // root folder and permissions on the PC; left out when undefined
        libraries: usedLibraries,
        natives: [...natives].sort(),
        scripts,
    };
    entries.unshift({ name: "manifest.json", data: encodeText(JSON.stringify(manifest, null, 1)) });

    const output = join(directory, project.output);
    fs.mkdirSync(output, { recursive: true });
    const file = join(output, `${project.name}-${buildId.slice(0, 12)}.qjspkg`);
    const archive = writeZip(entries);
    fs.writeFileSync(file, archive);
    prune(output, project.name, project.keep, file);
    return { file, buildId, scriptCount: scripts.length, size: archive.length, warnings, notes };
}
