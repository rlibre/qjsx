/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "stdlib/path" of the standard library (lib/stdlib).
//
// Behaves like Node's path.posix; on Windows it also accepts '\' as a
// separator and drive letters ("C:/dir"). Results always use '/', which
// every Windows API accepts. Differs from Node on Windows, where Node's
// default 'path' produces '\'.

const isWindows = process.platform === "win32";

export const sep = "/";
export const delimiter = isWindows ? ";" : ":";

/**
 * Throws a TypeError unless the argument named 'name' is a string (like Node).
 */
function checkString(value, name) {
    if (typeof value !== "string") {
        throw new TypeError(`path: ${name} must be a string`);
    }
}

/**
 * Checks that the path is a string and, on Windows, turns its '\' separators
 * into '/', so that the other functions only deal with '/'. A long path
 * ("\\?\...") or a device path ("\\.\COM10") is left as it is: Windows reads
 * it without normalizing, so it needs its '\'.
 */
function toSlashes(path) {
    checkString(path, "path");
    if (!isWindows) {
        return path;
    }

    const keepsBackslashes = path.startsWith("\\\\?\\") || path.startsWith("\\\\.\\");
    if (keepsBackslashes) {
        return path;
    }

    return path.replace(/\\/g, "/");
}

/**
 * Length of the root: "/" (1), "C:/" (3), "C:" (2, drive-relative) or none (0).
 */
function rootLength(path) {
    if (isWindows && /^[A-Za-z]:\//.test(path)) {
        return 3;
    }

    if (isWindows && /^[A-Za-z]:/.test(path)) {
        return 2;
    }

    return path.startsWith("/") ? 1 : 0;
}

/**
 * Resolves "." and ".." in a path without its root; returns the segments
 * joined by '/' (no leading or trailing '/').
 */
function normalizeSegments(path, allowAboveRoot) {
    const result = [];

    for (const segment of path.split("/")) {
        // empty segments (repeated '/') and "." change nothing
        if (segment === "" || segment === ".") {
            continue;
        }

        // ".." removes the previous segment; when it cannot, it is kept if
        // allowAboveRoot (relative paths), dropped otherwise ("/.." is "/")
        if (segment === "..") {
            const canGoUp = result.length > 0 && result[result.length - 1] !== "..";
            if (canGoUp) {
                result.pop();
            } else if (allowAboveRoot) {
                result.push("..");
            }

            continue;
        }

        // any other segment is kept
        result.push(segment);
    }

    return result.join("/");
}

/**
 * path.normalize(path), like Node's: resolves "." and "..", collapses
 * repeated separators, keeps a trailing '/'.
 */
export function normalize(path) {
    // "" gives "."
    path = toSlashes(path);
    if (path.length === 0) {
        return ".";
    }

    // keep the root as it is and normalize the rest; after no root or a
    // drive-relative "C:", ".." may go above the start; nothing left is "."
    const root = path.slice(0, rootLength(path));
    const isRelative = root.length === 0 || root.length === 2;
    let body = normalizeSegments(path.slice(root.length), isRelative);
    if (body.length === 0 && root.length === 0) {
        body = ".";
    }

    // keep a trailing '/'
    if (body.length > 0 && path.endsWith("/")) {
        body += "/";
    }

    return root + body;
}

/**
 * path.isAbsolute(path), like Node's: starts with '/', or on Windows with '\'
 * or with a drive and a separator ("C:/"); "C:dir" is not absolute.
 */
export function isAbsolute(path) {
    path = toSlashes(path);
    return path.startsWith("/") || (isWindows && /^[A-Za-z]:\//.test(path));
}

/**
 * path.join(...paths), like Node's: joins the non-empty parts with '/' and
 * normalizes the result; "." if there is nothing to join.
 */
export function join(...paths) {
    const parts = paths.map(toSlashes).filter(part => part.length > 0);
    return parts.length === 0 ? "." : normalize(parts.join("/"));
}

/**
 * path.resolve(...paths), like Node's: an absolute path without trailing '/'.
 */
export function resolve(...paths) {
    // prepend the parts from right to left until the result is absolute
    // (empty parts are skipped)
    let resolved = "";

    for (let i = paths.length - 1; i >= 0 && !isAbsolute(resolved); i--) {
        const part = toSlashes(paths[i]);
        if (part.length > 0) {
            resolved = resolved.length > 0 ? part + "/" + resolved : part;
        }
    }

    // still relative: start from the current directory
    if (!isAbsolute(resolved)) {
        resolved = process.cwd() + (resolved.length > 0 ? "/" + resolved : "");
    }

    // resolve "." and ".." below the root (they cannot go above it)
    const root = resolved.slice(0, rootLength(resolved));
    return root + normalizeSegments(resolved.slice(root.length), false);
}

/**
 * path.dirname(path), like Node's: the path without its last segment.
 */
export function dirname(path) {
    // "" gives "."
    path = toSlashes(path);
    if (path.length === 0) {
        return ".";
    }

    // ignore the trailing '/', never going into the root
    const root = path.slice(0, rootLength(path));
    let end = path.length;

    while (end > root.length && path[end - 1] === "/") {
        end--;
    }

    // find the last '/' before them: if it is inside the root (or there is
    // none), the result is the root, or "." for a relative path
    const slash = path.lastIndexOf("/", end - 1);
    if (slash < root.length) {
        return root.length > 0 ? root : ".";
    }

    // otherwise cut there and drop the '/' that precede the cut
    let directory = path.slice(0, slash);

    while (directory.length > root.length && directory.endsWith("/")) {
        directory = directory.slice(0, -1);
    }

    return directory.length > 0 ? directory : root;
}

/**
 * path.basename(path, extension), like Node's: the last segment, without
 * 'extension' if it ends the name (and is not the whole name).
 */
export function basename(path, extension) {
    // ignore the trailing '/'
    path = toSlashes(path);
    let end = path.length;

    while (end > 0 && path[end - 1] === "/") {
        end--;
    }

    // take the last segment, without the drive on Windows ("C:file" gives
    // "file")
    const start = path.lastIndexOf("/", end - 1) + 1;
    let base = path.slice(start, end);
    if (isWindows && start === 0 && /^[A-Za-z]:/.test(base)) {
        base = base.slice(2);
    }

    // remove the extension if it ends the name without being the whole name
    if (extension !== undefined && base !== extension && base.endsWith(extension)) {
        base = base.slice(0, -extension.length);
    }

    return base;
}

/**
 * path.extname(path), like Node's: the last segment from its last '.'; "" if
 * there is no '.' or only a leading one (".bashrc").
 */
export function extname(path) {
    const base = basename(path);
    const dot = base.lastIndexOf(".");
    return dot <= 0 ? "" : base.slice(dot);
}

/**
 * path.relative(from, to), like Node's: the path that leads from 'from' to
 * 'to'.
 */
export function relative(from, to) {
    // resolve both to absolute paths; "" if they are equal
    from = resolve(from);
    to = resolve(to);
    if (from === to) {
        return "";
    }

    // different roots (drives, compared without case) have no relative path:
    // 'to' is returned as it is
    const fromRoot = from.slice(0, rootLength(from));
    const toRoot = to.slice(0, rootLength(to));
    if (fromRoot.toLowerCase() !== toRoot.toLowerCase()) {
        return to;   // different drives: no relative path
    }

    // skip the leading segments they have in common (compared WITH case)
    const fromParts = from.slice(fromRoot.length).split("/").filter(part => part.length > 0);
    const toParts = to.slice(toRoot.length).split("/").filter(part => part.length > 0);
    let common = 0;

    while (common < fromParts.length && common < toParts.length && fromParts[common] === toParts[common]) {
        common++;
    }

    // go up once for each remaining segment of 'from', then down the
    // remaining segments of 'to'
    const up = fromParts.slice(common).map(() => "..");
    return up.concat(toParts.slice(common)).join("/");
}

export default { sep, delimiter, normalize, isAbsolute, join, resolve, dirname, basename, extname, relative };
