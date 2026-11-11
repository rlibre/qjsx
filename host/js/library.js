/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Libraries of JS modules: .qjslib files, made by 'qjsx mklib' and used by
// 'qjsx build'.
//
// A device has no compiler and its firmware embeds no JS: a program brings
// the library modules it imports in its own package. 'qjsx build' copies
// them there, whole, from the libraries.
//
// Names: every module of the library "sensors" is named "sensors/<module>",
// and imported by that name (import { read } from "sensors/bme280"). The
// standard library is "stdlib" (stdlib.qjslib, next to the executable). A
// module of a library imports its neighbours by their full name or
// relatively ("./filter" from "sensors/bme280" is "sensors/filter").
//
// Library content (zip, entries stored), laid out like a package:
//   manifest.json            format, kind "library", name, version, created,
//                            compiler, bytecode (version), natives[], modules[]
//   bytecode/<module>.qbc    bytecode of the module: a cache, only used by a
//                            build whose compiler is the one that made it
//                            (same compiler id and same bytecode version)
//   debug/<module>.json      its debug sidecar (same condition)
//   src/<module>.js          its source: what a build with another compiler
//                            compiles again, and what the debugger shows
//
// manifest.modules[]: { name ("sensors/bme280"), buildId, sourceHash,
// bytecode, debug, source (entry names), imports (module names) }.
// manifest.natives[]: the native modules the library imports ("fs"...): a
// name without '/' that is not a module of a library.
import * as fs from "fs";
import { join, dirname } from "stdlib/path";
import { writeZip, readZip } from "qjsx:zip";
import { loadProject, PROJECT_FILE } from "qjsx:project";
import { encodeText } from "qjsx:util";
import { compileModule, compilerId, BYTECODE_VERSION } from "qjsx:compile";

export const LIBRARY_FORMAT = 1;
export const LIBRARY_EXTENSION = ".qjslib";
export const STANDARD_LIBRARY = "stdlib";

const decoder = new TextDecoder();

// Where the standard library is: next to the executable (process.argv[0] is
// its full path).
export function standardLibraryFile() {
    return join(dirname(process.argv[0]), `${STANDARD_LIBRARY}${LIBRARY_EXTENSION}`);
}

// Library a module name belongs to ("stdlib/path" -> "stdlib"); undefined
// for a name without '/', which can only be a native module or a module at
// the root of a program.
export function libraryOf(moduleName) {
    const slash = moduleName.indexOf("/");
    return slash < 0 ? undefined : moduleName.slice(0, slash);
}

// Compiles the modules of a library project (qjsx.json with 'modules') and
// writes its .qjslib file: 'outputFile', or <output>/<name>.qjslib in the
// project. Returns { file, name, version, moduleCount, size, warnings };
// 'warnings' has one { file, kind, line, column } per use of eval() or of the
// Function constructor in the sources (qjsx:compile, warningText).
export function makeLibrary(directory, outputFile) {
    const project = loadProject(directory);
    const projectFile = join(directory, PROJECT_FILE);
    if (project.modules === undefined || Object.keys(project.modules).length === 0) {
        throw new Error(`${projectFile}: a library needs 'modules' (module name -> source file)`);
    }
    if (project.version === undefined) {
        throw new Error(`${projectFile}: a library needs a 'version'`);
    }

    // compile each module under its full name
    const modules = [];
    const entries = [];
    const warnings = [];
    for (const [key, sourcePath] of Object.entries(project.modules)) {
        const name = `${project.name}/${key}`;
        const source = fs.readFileSync(join(directory, sourcePath), "utf8");
        const compiled = compileModule(name, source);
        warnings.push(...compiled.warnings.map(warning => ({ file: sourcePath, ...warning })));
        const module = {
            name,
            buildId: compiled.buildId,
            sourceHash: compiled.sourceHash,
            bytecode: `bytecode/${key}.qbc`,
            debug: `debug/${key}.json`,
            source: `src/${key}.js`,
            imports: compiled.imports.map(imported => imported.name),
        };
        modules.push(module);
        entries.push({ name: module.bytecode, data: compiled.bytecode });
        entries.push({ name: module.debug, data: encodeText(JSON.stringify(compiled.sidecar, null, 1)) });
        entries.push({ name: module.source, data: encodeText(source) });
    }

    // what the modules import: a module of this library must exist; a name
    // without '/' is a native module; the rest belongs to other libraries,
    // which the program that uses this one must declare
    const names = new Set(modules.map(module => module.name));
    const natives = new Set();
    for (const module of modules) {
        for (const imported of module.imports) {
            const libraryName = libraryOf(imported);
            if (libraryName === project.name && !names.has(imported)) {
                throw new Error(`${module.name} imports '${imported}', which is not among the 'modules' of ${projectFile}`);
            }
            if (libraryName === undefined) {
                natives.add(imported);
            }
        }
    }

    const manifest = {
        format: LIBRARY_FORMAT,
        kind: "library",
        name: project.name,
        version: project.version,
        created: new Date().toISOString(),
        compiler: compilerId(),
        bytecode: BYTECODE_VERSION,
        natives: [...natives].sort(),
        modules,
    };
    entries.unshift({ name: "manifest.json", data: encodeText(JSON.stringify(manifest, null, 1)) });

    const file = outputFile ?? join(directory, project.output, `${project.name}${LIBRARY_EXTENSION}`);
    fs.mkdirSync(dirname(file), { recursive: true });
    const archive = writeZip(entries);
    fs.writeFileSync(file, archive);
    return {
        file,
        name: project.name,
        version: project.version,
        moduleCount: modules.length,
        size: archive.length,
        warnings,
    };
}

// Opens a .qjslib file. Returns { file, name, version, compiler, bytecode,
// modules, entries }: 'modules' is a Map(module name -> its record of the
// manifest), 'entries' the content of the archive (Map(entry name -> bytes)).
export function readLibrary(file) {
    const entries = readZip(fs.readFileSync(file));
    const manifestBytes = entries.get("manifest.json");
    if (manifestBytes === undefined) {
        throw new Error(`${file}: not a library (no manifest.json)`);
    }
    const manifest = JSON.parse(decoder.decode(manifestBytes));
    if (manifest.kind !== "library") {
        throw new Error(`${file}: not a library (its manifest is not of kind "library")`);
    }
    if (manifest.format !== LIBRARY_FORMAT) {
        throw new Error(`${file}: library format ${manifest.format} is not supported (this qjsx reads format ${LIBRARY_FORMAT})`);
    }

    const modules = new Map(manifest.modules.map(module => [module.name, module]));
    return {
        file,
        name: manifest.name,
        version: manifest.version,
        compiler: manifest.compiler,
        bytecode: manifest.bytecode,
        modules,
        entries,
    };
}

function entryOf(library, entryName) {
    const bytes = library.entries.get(entryName);
    if (bytes === undefined) {
        throw new Error(`${library.file}: entry '${entryName}' is missing`);
    }
    return bytes;
}

// True if the bytecode and the sidecars stored in the library can be used as
// they are: they were made by this compiler (same compiler id, same bytecode
// version). Else its modules are compiled again from their sources.
export function madeByThisCompiler(library) {
    return library.compiler === compilerId() && library.bytecode === BYTECODE_VERSION;
}

// A module of a library, ready to go into a package: { source, buildId,
// sourceHash, bytecode, sidecar, imports, warnings }, like compileModule()
// plus the source text. The bytecode and the sidecar stored in the library
// are only valid for the compiler that made them (madeByThisCompiler): with
// another one, the module is compiled again from its source. A stored module
// gives no warning: 'qjsx mklib' gave them when the library was made.
export function libraryModule(library, name) {
    const record = library.modules.get(name);
    const source = decoder.decode(entryOf(library, record.source));
    if (!madeByThisCompiler(library)) {
        return { source, ...compileModule(name, source) };
    }

    return {
        source,
        buildId: record.buildId,
        sourceHash: record.sourceHash,
        bytecode: entryOf(library, record.bytecode),
        sidecar: JSON.parse(decoder.decode(entryOf(library, record.debug))),
        imports: record.imports.map(imported => ({ specifier: imported, name: imported })),
        warnings: [],
    };
}
