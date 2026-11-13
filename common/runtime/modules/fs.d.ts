/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * Synchronous file system functions, named and behaving like their Node
 * counterparts (a subset; the differences are given for each function).
 *
 * Paths are strings (no Buffer, no URL). A relative path resolves against
 * `process.cwd()`: the root folder of a PC program, the data folder of a
 * device, else the system's current folder.
 *
 * Permissions: a program of the PC reaches only what it was granted
 * (`qjsx run -R` / `--allow-read[=folders]`, `-W` / `--allow-write[=folders]`,
 * or the `desktop` section of `qjsx.json`); reading needs `read` access,
 * creating, writing, renaming and deleting need `write` access. A refused
 * access throws an Error with `code` `"EACCES"` whose message says how to
 * grant it. The desktop agent and the twin reach only their data folder
 * (`<storage>/data`).
 *
 * Errors are `Error` objects with Node-like properties `code` (`"ENOENT"`,
 * `"EACCES"`, `"EEXIST"`, `"EISDIR"`, `"ENOTDIR"`, `"ENOTEMPTY"`...;
 * `"EUNKNOWN"` for a system error qjsx has no name for) and `errno` (the
 * number), and a message `"<code>: <system text>, <path>"` naming the path
 * the program gave.
 *
 * @example
 * import { readFileSync, writeFileSync } from "sys/fs";
 * writeFileSync("settings.json", JSON.stringify({ speed: 3 }));
 * const settings = JSON.parse(readFileSync("settings.json", "utf8"));
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | ✓ what the program was granted | ✓ ¹ | ✓ ¹ | ✓ its data folder |
 *
 * ¹ No file system on the board yet: every function throws (errno ENOSYS,
 * `code` `"EUNKNOWN"`) except `existsSync`, which returns false.
 *
 * @module sys/fs
 */
declare module "sys/fs" {
    /**
     * Encodings `readFileSync` knows: UTF-8 only.
     */
    export type Encoding = "utf8" | "utf-8";

    /**
     * Bytes accepted by `writeFileSync`: any typed array (its own bytes, not
     * its elements) or an ArrayBuffer. A DataView or an array of numbers is
     * refused.
     */
    export type BinaryData = ArrayBuffer | Int8Array | Uint8Array | Uint8ClampedArray | Int16Array | Uint16Array | Int32Array | Uint32Array | Float16Array | Float32Array | Float64Array | BigInt64Array | BigUint64Array;

    /**
     * What `statSync` says of a path (a subset of Node's `fs.Stats`).
     * Symbolic links are followed.
     */
    export class Stats {
        /**
         * An empty Stats (size and time 0, neither file nor directory).
         * Programs get Stats from {@link statSync}; there is no reason to make
         * one.
         */
        constructor();

        /**
         * Size in bytes.
         */
        size: number;

        /**
         * Time of the last modification, in milliseconds since 1970-01-01 UTC,
         * with a resolution of one second (always a multiple of 1000).
         */
        mtimeMs: number;

        /**
         * True for a regular file.
         */
        isFile(): boolean;

        /**
         * True for a directory.
         */
        isDirectory(): boolean;
    }

    /**
     * Reads a whole file as text: with an encoding (`"utf8"` or `"utf-8"`,
     * alone or as `{ encoding }`), the content is decoded as UTF-8.
     *
     * @throws Error with `code` (`"ENOENT"`, `"EACCES"`, `"EISDIR"`...) if
     * the file cannot be read; TypeError for any other encoding name, checked
     * before the file is opened.
     */
    export function readFileSync(path: string, options: Encoding | { encoding: Encoding }): string;
    /**
     * Reads a whole file as bytes, in a new Uint8Array (Node returns a
     * Buffer): without options, or with options that give no encoding.
     *
     * @throws Error with `code` (`"ENOENT"`, `"EACCES"`, `"EISDIR"`...) if
     * the file cannot be read.
     */
    export function readFileSync(path: string, options?: { encoding?: null } | null): Uint8Array;

    /**
     * Creates or replaces the file with `data`: a string is written as UTF-8,
     * bytes as they are. Unlike Node, there are no options (a third argument
     * is ignored: no `flag`, `mode` or `encoding`), and an array of numbers is
     * refused.
     *
     * @throws Error with `code` (`"ENOENT"` when the folder does not exist,
     * `"EACCES"`...) if the file cannot be written; TypeError if `data` is
     * neither a string nor bytes.
     */
    export function writeFileSync(path: string, data: string | BinaryData): void;

    /**
     * True if something exists at the path (file, directory...).
     *
     * Unlike Node, it throws when the program may not read the path (Error
     * with `code` `"EACCES"`); any other failure gives false, as on a device
     * without file system.
     */
    export function existsSync(path: string): boolean;

    /**
     * Describes the path (links followed).
     *
     * @throws Error with `code` (`"ENOENT"`, `"EACCES"`...) if the path does
     * not exist or cannot be read.
     */
    export function statSync(path: string): Stats;

    /**
     * The names of the entries of a directory, without `"."` and `".."`, in
     * the file system's order (not sorted). No options (`withFileTypes`...).
     *
     * @throws Error with `code` (`"ENOENT"`, `"ENOTDIR"`, `"EACCES"`...).
     */
    export function readdirSync(path: string): string[];

    /**
     * Creates a directory.
     *
     * @param options `recursive` (default false): also create the missing
     * parents, and an existing directory is no error. Unknown fields are a
     * TypeError.
     * @throws Error with `code` (`"EEXIST"` without `recursive`, `"ENOENT"`
     * when a parent is missing, `"EACCES"`...). Unlike Node, it returns
     * nothing (Node returns the first folder created with `recursive`).
     */
    export function mkdirSync(path: string, options?: { recursive?: boolean }): void;

    /**
     * Deletes a file, or a directory with everything in it when `recursive`.
     * A symbolic link (or a Windows junction) inside the tree is removed
     * itself, never followed. On failure, what was already deleted stays
     * deleted.
     *
     * @param options `recursive` (default false): also delete a directory and
     * its content; `force` (default false): no error when the path does not
     * exist. Unknown fields are a TypeError.
     * @throws Error with `code`: `"ENOENT"` without `force`, `"EISDIR"` for a
     * directory without `recursive` (Node's code is `ERR_FS_EISDIR`),
     * `"EACCES"`...
     */
    export function rmSync(path: string, options?: { recursive?: boolean; force?: boolean }): void;

    /**
     * Renames or moves `from` to `to`, replacing `to` if it exists. Both
     * paths need `write` access.
     *
     * @throws Error with `code`; the message names `from`, like Node.
     */
    export function renameSync(from: string, to: string): void;

    /**
     * Deletes a file.
     *
     * @throws Error with `code` (`"ENOENT"`, `"EACCES"`...).
     */
    export function unlinkSync(path: string): void;
}
