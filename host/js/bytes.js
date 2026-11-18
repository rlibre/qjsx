/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Imports of bytes: a program imports a file of the project as its bytes,
//
//   import inter16 from "./fonts/inter16.bin" with { type: "bytes" };
//
// and gets a Uint8Array over them ('default', the only export). The file is
// copied as it is into the package, entry "bytes/<module name>", stored and
// aligned on 8 bytes: a device makes the array over the entry where the
// package lies (flash), without copying it into its RAM
// (common/runtime/program.cpp). The module is named like the modules of the
// program, by its path in the project, resolved against its importer.
//
// The manifest lists the files under "bytes": [{ path, size, hash }], and
// their content is part of the build id of the package.
//
// A module of a library imports bytes the same way: "./inter16.bin" in the
// module "stdlib/fonts/inter16" is the module "stdlib/fonts/inter16.bin".
// 'qjsx mklib' reads the file next to the source of the module (the same
// relative specifier, resolved against the source file) and stores it in the
// .qjslib, entry "bytes/<module name>", aligned like here; the manifest of
// the library lists it (qjsx:library). 'qjsx build' takes it from there, and
// only for a library module that is in the package: a file that no packaged
// module imports never goes into a package. Its name must stay in the
// library ("stdlib/..."). The tool's own copy of the standard library embeds
// these files too (tools/embed-bytes.lua), for 'qjsx run script.js'.
//
// An image (user, 2026-11-18) is imported the same way, with { type: "image" }:
//
//   import logo from "./logo.png" with { type: "image" };
//
// the PNG file is converted here (qjsx:image) into the format the renderer of
// the displays reads in place, and that is what goes into the package, under
// the same entry and in the same list as the bytes; its size and hash are
// those of the converted image. On the device both are the same: a module
// whose 'default' is a Uint8Array over the entry.
//
// Known limit: the bytes are read-only, but nothing says so to the program
// (the engine has no immutable ArrayBuffer): writing into the array is an
// error that is not detected (on a device the memory is flash).
import * as fs from "sys/fs";
import { join } from "stdlib/path";
import { sha256 } from "sys/crypto";
import { hex } from "qjsx:util";
import { imageFromPng } from "qjsx:image";

// The values of the attribute 'type' that import a file: as its bytes, or as
// an image converted for the displays.
export const BYTES_TYPE = "bytes";
export const IMAGE_TYPE = "image";
const IMPORT_TYPES = [BYTES_TYPE, IMAGE_TYPE];

// Prefix of the entries of the package that hold the files, and their
// alignment in the archive (a device reads them where the package lies).
export const BYTES_PREFIX = "bytes/";
export const BYTES_ALIGNMENT = 8;

// The outline of a module of bytes (see host/modules/compiler_module.cpp),
// for qjsx:prune: it exports 'default' only, declares and imports nothing.
const BYTES_OUTLINE = {
    statements: [],
    imports: [],
    exports: [{ exported: "default", local: "*default*", module: null, start: 0, end: 0 }],
    requests: [],
    hasEval: false,
};

/**
 * What the import attributes of an import ask for: null for a module (no
 * 'with' clause: 'attributes' null), BYTES_TYPE or IMAGE_TYPE for a file.
 * Any other attribute or type is an error at 'place' ("file:line:column").
 */
export function importType(attributes, place) {
    // no 'with' clause: a module
    if (attributes === null) {
        return null;
    }

    // 'type' is the only attribute
    const known = IMPORT_TYPES.map(type => `"${type}"`).join(" or ");
    for (const key of Object.keys(attributes)) {
        if (key !== "type") {
            throw new Error(`${place}: unsupported import attribute '${key}' (the only one is type: ${known})`);
        }
    }

    // and one of the known types its value
    if (!IMPORT_TYPES.includes(attributes.type)) {
        throw new Error(`${place}: unsupported import type '${attributes.type}' (known: ${known})`);
    }

    return attributes.type;
}

/**
 * The file of an import of bytes or of an image ({ specifier, name, type,
 * place } of compileModule().imports) in the project 'directory', at 'path'
 * in the project: the module name for a program (named by their paths), the
 * path next to the source file for a library (see the top of the file).
 * Returns { name, data, record }: 'name' is the module name, 'data' the
 * bytes of the file (an image: converted), 'record' its record of the
 * manifest of a package ({ path: the module name, size, hash }). The
 * specifier must be relative, the file inside the project.
 */
export function readBytesFile(directory, path, imported) {
    // "./file.bin" or "../file.bin", relative to its importer
    if (!imported.specifier.startsWith(".")) {
        throw new Error(`${imported.place}: an import of bytes names a file relative to its module ("./file.bin"), not '${imported.specifier}'`);
    }

    // a path that goes up out of the project is refused (the normalizer
    // keeps the ".." it cannot resolve)
    const leavesProject = path.split("/").includes("..");
    if (leavesProject) {
        throw new Error(`${imported.place}: '${imported.specifier}' is outside the project`);
    }

    // the file must exist
    const file = join(directory, path);
    const isFile = fs.existsSync(file) && fs.statSync(file).isFile();
    if (!isFile) {
        throw new Error(`${imported.place}: no file '${path}' for this import of bytes`);
    }

    // its bytes, as they are or converted when an image, and what the
    // manifest says of them
    const data = fs.readFileSync(file);
    const packaged = imported.type === IMAGE_TYPE ? convertImage(data, path, imported.place) : data;
    return bytesFile(imported.name, packaged, hex(sha256(packaged)));
}

/**
 * The image file 'path' (its bytes: 'data') converted for the displays
 * (qjsx:image). A failure says which file and where it is imported.
 */
function convertImage(data, path, place) {
    try {
        return imageFromPng(data);
    } catch (error) {
        throw new Error(`${place}: the image '${path}' cannot be converted: ${error.message}`);
    }
}

/**
 * A file imported as bytes, as readBytesFile() gives it: { name, data,
 * record }, from its module name, its bytes and their sha256 in hex.
 */
export function bytesFile(name, data, hash) {
    const record = { path: name, size: data.length, hash };
    return { name, data, record };
}

/**
 * A module of bytes as qjsx:prune reads the modules of a package: no source,
 * no import, 'default' as its only export.
 */
export function bytesModule(name) {
    return { script: { path: name }, source: "", imports: [], outline: BYTES_OUTLINE };
}

/**
 * The entry of the package that holds a file read by readBytesFile().
 */
export function bytesEntry(bytesFile) {
    return { name: `${BYTES_PREFIX}${bytesFile.name}`, data: bytesFile.data, align: BYTES_ALIGNMENT };
}
