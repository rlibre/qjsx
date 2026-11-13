/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * Paths, from the standard library (written in JS): the functions of Node's
 * `path` module with the semantics of Node's `path.posix`.
 *
 * On Windows (`process.platform === "win32"`) it also accepts `\` as a
 * separator and drive letters (`"C:/dir"`, `"C:\\dir"`, drive-relative
 * `"C:dir"`), and its results ALWAYS use `/`, which every Windows API
 * accepts: this differs from Node's default `path` on Windows, which produces
 * `\`. A long path (`\\?\...`) or a device path (`\\.\COM10`) keeps its `\`
 * untouched: the functions only see `/`, so such a path is treated as one
 * name. Elsewhere (Linux, macOS, the devices) `\` is an ordinary character
 * of a name and there are no drive letters.
 *
 * Every function throws a `TypeError` ("path: path must be a string") when a
 * path argument is not a string.
 *
 * Not provided (unlike Node): `parse`, `format`, `toNamespacedPath`,
 * `matchesGlob`, `posix`, `win32`.
 *
 * @example
 * import { join, dirname, basename, extname } from "stdlib/path";
 * import path from "stdlib/path";             // the same functions, as an object
 *
 * join("data", "logs", "..", "today.txt");    // "data/today.txt"
 * dirname("/data/logs/today.txt");            // "/data/logs"
 * basename("/data/today.txt", ".txt");        // "today"
 * extname("archive.tar.gz");                  // ".gz"
 * path.resolve("config.json");                // process.cwd() + "/config.json"
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | ✓ | ✓ | ✓ | ✓ |
 *
 * @module stdlib/path
 */
declare module "stdlib/path" {
    /**
     * The separator of the results: always `"/"`, Windows included (Node's
     * `path.sep` is `"\\"` on Windows).
     */
    export const sep: "/";

    /**
     * The separator of the entries of a list of paths (such as `PATH`):
     * `";"` on Windows, `":"` everywhere else (devices included), as Node's.
     */
    export const delimiter: ";" | ":";

    /**
     * Resolves `.` and `..`, collapses repeated separators and keeps a
     * trailing `/`, like Node's `path.posix.normalize`. `""` gives `"."`.
     * The root is kept as written (`"/"`, `"C:/"`); `..` cannot go above an
     * absolute root (`"/a/../.."` gives `"/"`) but is kept at the start of a
     * relative or drive-relative path (`"../a"`, `"C:../a"`). On Windows the
     * `\` separators become `/`.
     *
     * @example
     * normalize("/data//logs/../today.txt");  // "/data/today.txt"
     * normalize("a/./b/");                     // "a/b/"
     */
    export function normalize(path: string): string;

    /**
     * Whether the path is absolute: it starts with `/`, or on Windows with
     * `\` or with a drive followed by a separator (`"C:/"`, `"C:\\"`).
     * A drive-relative path (`"C:dir"`) is not absolute.
     */
    export function isAbsolute(path: string): boolean;

    /**
     * Joins the non-empty parts with `/` and normalizes the result (see
     * {@link normalize}), like Node's `path.join`; `"."` when there is
     * nothing to join. A trailing `/` of the last part is kept.
     *
     * @example
     * join("/data", "logs/", "../today.txt");  // "/data/today.txt"
     * join();                                   // "."
     */
    export function join(...paths: string[]): string;

    /**
     * An absolute path, like Node's `path.resolve`: the parts are taken from
     * right to left (empty ones skipped) until the result is absolute, then
     * prefixed by `process.cwd()` if it still is not; `.` and `..` are
     * resolved (never above the root) and there is no trailing `/`.
     * `resolve()` gives `process.cwd()`.
     *
     * `process.cwd()` is the program's data folder on a device
     * (`<storage>/data` for the desktop agent and the twin), the root
     * folder of a PC program that declares one, else the current folder.
     *
     * Differences from Node on Windows: a path rooted without a drive
     * (`"/a"`) stays without a drive (Node adds the current one), and a
     * drive-relative path (`"C:a"`) is not understood as such: it is
     * appended to `process.cwd()` as a name.
     */
    export function resolve(...paths: string[]): string;

    /**
     * The path without its last segment, like Node's `path.dirname`;
     * trailing separators are ignored. A root gives itself (`"/"`, `"C:/"`),
     * a single relative name `"."`, `""` gives `"."`.
     *
     * @example
     * dirname("/data/logs/");   // "/data"
     * dirname("today.txt");     // "."
     */
    export function dirname(path: string): string;

    /**
     * The last segment of the path, trailing separators ignored, like
     * Node's `path.basename`; on Windows without a leading drive
     * (`"C:file"` gives `"file"`).
     *
     * @param suffix removed from the result when the name ends with it and
     *     is not the whole name (`basename("a.js", ".js")` is `"a"`,
     *     `basename(".js", ".js")` is `".js"`). It must be a string: unlike
     *     Node, another type is not checked and gives a meaningless result.
     */
    export function basename(path: string, suffix?: string): string;

    /**
     * The extension of the last segment, from its last `.`, like Node's
     * `path.extname`: `""` when there is no `.` or only a leading one
     * (`".bashrc"`), `"."` for a name ending with a dot.
     *
     * @example
     * extname("index.html");   // ".html"
     * extname("a.tar.gz");     // ".gz"
     */
    export function extname(path: string): string;

    /**
     * The path that leads from `from` to `to`, like Node's `path.relative`:
     * both are first resolved (see {@link resolve}); `""` when they are the
     * same. The result uses `/` and has no trailing `/`.
     *
     * On Windows, two different drives (compared without case) have no
     * relative path: the resolved `to` is returned. The segments are
     * compared WITH case (Node's `path.win32` compares them without case).
     *
     * @example
     * relative("/data/logs", "/data/config/app.json");   // "../config/app.json"
     */
    export function relative(from: string, to: string): string;

    /**
     * The shape of the default export: the module's constants and functions
     * in one object, as `import path from "stdlib/path"` gives them.
     */
    export interface PathModule {
        /** See {@link sep}. */
        sep: "/";

        /** See {@link delimiter}. */
        delimiter: ";" | ":";

        /** See {@link normalize}. */
        normalize(path: string): string;

        /** See {@link isAbsolute}. */
        isAbsolute(path: string): boolean;

        /** See {@link join}. */
        join(...paths: string[]): string;

        /** See {@link resolve}. */
        resolve(...paths: string[]): string;

        /** See {@link dirname}. */
        dirname(path: string): string;

        /** See {@link basename}. */
        basename(path: string, suffix?: string): string;

        /** See {@link extname}. */
        extname(path: string): string;

        /** See {@link relative}. */
        relative(from: string, to: string): string;
    }

    /** The default export's value (declared for the line below). */
    const path: PathModule;

    /**
     * The constants and functions of the module in one plain object, so that
     * `import path from "stdlib/path"` reads as with Node
     * (`path.join(...)`).
     */
    export default path;
}
