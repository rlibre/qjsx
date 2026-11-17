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
//   outline/<module>.json    its outline: what 'qjsx build' reads to leave
//                            out of a package what nothing uses (qjsx:prune;
//                            same condition)
//   src/<module>.js          its source: what a build with another compiler
//                            compiles again, and what the debugger shows
//   bytes/<name>             a file a module imports as bytes, named by its
//                            module name ("stdlib/fonts/inter16.bin"), as it
//                            is, aligned on 8 bytes (qjsx:bytes)
//
// manifest.modules[]: { name ("sensors/bme280"), buildId, sourceHash,
// bytecode, debug, outline, source (entry names), imports ([{ name, bytes }]:
// the modules it imports, 'bytes' true for a file imported as bytes) }.
// A library made before the outlines has no 'outline': its modules are
// compiled again from their sources when a program uses them. One of format
// 1 (before the imports of bytes) lists its imports as plain names.
// manifest.natives[]: the native modules the library imports ("sys/fs",
// "device/gpio"...): see isNativeModule.
// manifest.bytes[]: { name, entry, size, hash } of each file imported as
// bytes (absent in format 1).
import * as fs from "sys/fs";
import { join, dirname } from "stdlib/path";
import { writeZip, readZip } from "qjsx:zip";
import { loadProject, PROJECT_FILE } from "qjsx:project";
import { encodeText } from "qjsx:util";
import { compileModule, compilerId, normalizeImport, BYTECODE_VERSION } from "qjsx:compile";
import { readBytesFile, bytesFile, bytesEntry } from "qjsx:bytes";

// The format this tool writes (2: the imports of bytes), and those it reads.
export const LIBRARY_FORMAT = 2;
const READABLE_FORMATS = [1, 2];
export const LIBRARY_EXTENSION = ".qjslib";
export const STANDARD_LIBRARY = "stdlib";

const decoder = new TextDecoder();

/**
 * Where the standard library is: next to the executable (process.argv[0] is
 * its full path).
 */
export function standardLibraryFile() {
    return join(dirname(process.argv[0]), `${STANDARD_LIBRARY}${LIBRARY_EXTENSION}`);
}

/**
 * Library a module name belongs to ("stdlib/path" -> "stdlib"); undefined
 * for a name without '/'.
 */
export function libraryOf(moduleName) {
    const slash = moduleName.indexOf("/");
    return slash < 0 ? undefined : moduleName.slice(0, slash);
}

// The prefixes of the native modules (of the firmware, or of the tool on the
// PC): "sys/fs", "sys/net", "device/serial", "device/gpio"... No library may
// take one of these names.
export const NATIVE_PREFIXES = ["sys", "device"];

/**
 * True if a module name is the one of a native module (sys/..., device/...).
 */
export function isNativeModule(moduleName) {
    return NATIVE_PREFIXES.includes(libraryOf(moduleName));
}

/**
 * The error of an import of a name without '/' that no module has: native
 * modules are named with a prefix now ("fs" became "sys/fs").
 */
export function bareNameError(where, name) {
    return new Error(`${where}: no module '${name}' (native modules are named sys/<name> or device/<name>, e.g. 'sys/fs')`);
}

/**
 * The file that a module of the library project in 'directory' imports as
 * bytes ('imported', of compileModule().imports), as qjsx:bytes
 * readBytesFile() gives it. On the disk it is next to the source of the
 * module ('sourcePath', in the project), by the same relative specifier; its
 * module name must stay in the library 'libraryName'.
 */
function readLibraryBytes(directory, libraryName, sourcePath, imported) {
    // the file, relative to the source of the module
    const path = normalizeImport(sourcePath, imported.specifier);
    const file = readBytesFile(directory, path, imported);

    // its name, in the library: a program finds it among the library's files
    const insideLibrary = imported.name.startsWith(`${libraryName}/`) && !imported.name.split("/").includes("..");
    if (!insideLibrary) {
        throw new Error(`${imported.place}: '${imported.specifier}' is the module '${imported.name}', outside the library '${libraryName}'`);
    }

    return file;
}

/**
 * Compiles the modules of a library project (qjsx.json with 'modules') and
 * writes its .qjslib file: 'outputFile', or <output>/<name>.qjslib in the
 * project. The files the modules import as bytes go into it too. Returns
 * { file, name, version, moduleCount, size, warnings }; 'warnings' has one
 * { file, kind, line, column } per use of eval() or of the Function
 * constructor in the sources (qjsx:compile, warningText).
 */
export function makeLibrary(directory, outputFile) {
    // a library project: modules, a version, a name that is not a native prefix
    const project = loadProject(directory);
    const projectFile = join(directory, PROJECT_FILE);
    const lacksModules = project.modules === undefined || Object.keys(project.modules).length === 0;
    if (lacksModules) {
        throw new Error(`${projectFile}: a library needs 'modules' (module name -> source file)`);
    }

    if (project.version === undefined) {
        throw new Error(`${projectFile}: a library needs a 'version'`);
    }

    if (NATIVE_PREFIXES.includes(project.name)) {
        throw new Error(`${projectFile}: a library cannot be named '${project.name}', the prefix of the native modules`);
    }

    // compile each module under its full name
    const modules = [];
    const entries = [];
    const warnings = [];
    const bytesFiles = new Map();   // module name -> file imported as bytes (qjsx:bytes bytesFile)

    for (const [key, sourcePath] of Object.entries(project.modules)) {
        const name = `${project.name}/${key}`;
        const source = fs.readFileSync(join(directory, sourcePath), "utf8");
        const compiled = compileModule(name, source);
        warnings.push(...compiled.warnings.map(warning => ({ file: sourcePath, ...warning })));

        // the files it imports as bytes, each read once
        for (const imported of compiled.imports) {
            const isNewFile = imported.bytes && !bytesFiles.has(imported.name);
            if (isNewFile) {
                bytesFiles.set(imported.name, readLibraryBytes(directory, project.name, sourcePath, imported));
            }
        }

        // its record of the manifest
        const module = {
            name,
            buildId: compiled.buildId,
            sourceHash: compiled.sourceHash,
            bytecode: `bytecode/${key}.qbc`,
            debug: `debug/${key}.json`,
            outline: `outline/${key}.json`,
            source: `src/${key}.js`,
            imports: compiled.imports.map(imported => ({ name: imported.name, bytes: imported.bytes })),
        };
        modules.push(module);

        // its entries: bytecode, sidecar, outline and source
        entries.push({ name: module.bytecode, data: compiled.bytecode });
        entries.push({ name: module.debug, data: encodeText(JSON.stringify(compiled.sidecar, null, 1)) });
        entries.push({ name: module.outline, data: encodeText(JSON.stringify(compiled.outline)) });
        entries.push({ name: module.source, data: encodeText(source) });
    }

    // what the modules import: a module of this library must exist; sys/...
    // and device/... are native modules; a bare name is an error; the rest
    // belongs to other libraries, which the program that uses this one must
    // declare. A file imported as bytes is not a module of the library.
    const names = new Set(modules.map(module => module.name));
    const natives = new Set();

    for (const module of modules) {
        for (const { name: imported, bytes } of module.imports) {
            if (bytes) {
                if (names.has(imported)) {
                    throw new Error(`${module.name} imports '${imported}' as bytes, but it is a module of the library`);
                }
                continue;
            }

            const libraryName = libraryOf(imported);
            const isMissingNeighbour = libraryName === project.name && !names.has(imported);
            if (isMissingNeighbour) {
                throw new Error(`${module.name} imports '${imported}', which is not among the 'modules' of ${projectFile}`);
            }

            if (isNativeModule(imported)) {
                natives.add(imported);
            }

            if (libraryName === undefined) {
                throw bareNameError(`${module.name} imports '${imported}'`, imported);
            }
        }
    }

    // the files imported as bytes: their entries, aligned, and their records
    const bytesRecords = [];

    for (const file of bytesFiles.values()) {
        const entry = bytesEntry(file);
        entries.push(entry);
        bytesRecords.push({ name: file.name, entry: entry.name, size: file.record.size, hash: file.record.hash });
    }

    // the manifest, first entry of the archive
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
        bytes: bytesRecords,
    };
    entries.unshift({ name: "manifest.json", data: encodeText(JSON.stringify(manifest, null, 1)) });

    // write the archive
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

/**
 * Opens a .qjslib file. Returns { file, name, version, compiler, bytecode,
 * modules, bytes, entries }: 'modules' is a Map(module name -> its record of
 * the manifest), 'bytes' a Map(module name -> record of a file imported as
 * bytes), 'entries' the content of the archive (Map(entry name -> bytes)).
 */
export function readLibrary(file) {
    // its manifest
    const entries = readZip(fs.readFileSync(file));
    const manifestBytes = entries.get("manifest.json");
    if (manifestBytes === undefined) {
        throw new Error(`${file}: not a library (no manifest.json)`);
    }

    // a library, of a format this tool reads
    const manifest = JSON.parse(decoder.decode(manifestBytes));
    if (manifest.kind !== "library") {
        throw new Error(`${file}: not a library (its manifest is not of kind "library")`);
    }

    if (!READABLE_FORMATS.includes(manifest.format)) {
        throw new Error(`${file}: library format ${manifest.format} is not supported (this qjsx reads formats ${READABLE_FORMATS.join(", ")})`);
    }

    // its modules and its files of bytes (none before format 2) by name
    const modules = new Map(manifest.modules.map(module => [module.name, module]));
    const bytes = new Map((manifest.bytes ?? []).map(record => [record.name, record]));
    return {
        file,
        name: manifest.name,
        version: manifest.version,
        compiler: manifest.compiler,
        bytecode: manifest.bytecode,
        modules,
        bytes,
        entries,
    };
}

/**
 * The bytes of the entry 'entryName' of an open library; throws when missing.
 */
function entryOf(library, entryName) {
    const bytes = library.entries.get(entryName);
    if (bytes === undefined) {
        throw new Error(`${library.file}: entry '${entryName}' is missing`);
    }

    return bytes;
}

/**
 * True if the bytecode and the sidecars stored in the library can be used as
 * they are: they were made by this compiler (same compiler id, same bytecode
 * version). Else its modules are compiled again from their sources.
 */
export function madeByThisCompiler(library) {
    return library.compiler === compilerId() && library.bytecode === BYTECODE_VERSION;
}

/**
 * A module of a library, ready to go into a package: { source, buildId,
 * sourceHash, bytecode, sidecar, imports, warnings, outline }, like
 * compileModule() plus the source text. The bytecode, the sidecar and the
 * outline stored in the library are only valid for the compiler that made
 * them (madeByThisCompiler), and a library made before the outlines has
 * none: else the module is compiled again from its source. A stored module
 * gives no warning: 'qjsx mklib' gave them when the library was made.
 */
export function libraryModule(library, name) {
    const record = library.modules.get(name);
    const source = decoder.decode(entryOf(library, record.source));

    // stored bytecode of another compiler, or no outline: compiled again
    const mustCompile = !madeByThisCompiler(library) || record.outline === undefined;
    if (mustCompile) {
        return { source, ...compileModule(name, source) };
    }

    // else what the library holds
    return {
        source,
        buildId: record.buildId,
        sourceHash: record.sourceHash,
        bytecode: entryOf(library, record.bytecode),
        sidecar: JSON.parse(decoder.decode(entryOf(library, record.debug))),
        imports: record.imports.map(imported => storedImport(name, imported)),
        warnings: [],
        outline: JSON.parse(decoder.decode(entryOf(library, record.outline))),
    };
}

/**
 * An import of the library module 'importer' as compileModule() gives it,
 * from its record in the manifest: { name, bytes } (format 2), or a plain
 * name (format 1, which had no import of bytes). The name stands for the
 * specifier, the module for the place.
 */
function storedImport(importer, imported) {
    const isPlainName = typeof imported === "string";
    const name = isPlainName ? imported : imported.name;
    const bytes = isPlainName ? false : imported.bytes;
    return { specifier: name, name, bytes, place: importer };
}

/**
 * A file that a module of an open library imports as bytes ('imported', of
 * libraryModule().imports), as qjsx:bytes readBytesFile() gives one, taken
 * from the entries of the library. Throws when the library does not hold it.
 */
export function libraryBytesFile(library, imported) {
    const record = library.bytes.get(imported.name);
    if (record === undefined) {
        throw new Error(`${imported.place}: the library '${library.name}' (${library.file}) has no file '${imported.name}' imported as bytes`);
    }

    return bytesFile(imported.name, entryOf(library, record.entry), record.hash);
}
