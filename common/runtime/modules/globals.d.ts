/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * Globals of every qjsx program: `console`, the
 * timers, `TextEncoder` / `TextDecoder` (UTF-8 only) and `process`.
 *
 * Besides the standard ECMAScript objects (no browser or Node API), this is
 * all a program finds in `globalThis`. Everything else comes from modules:
 * `sys/...` and `device/...` (native) and `stdlib/...` (standard library).
 *
 * Output: on the PC `console` and `process.stdout` / `process.stderr` write
 * to the standard streams; on a device they go to the PC during a session
 * (`qjsx monitor`, `qjsx debug`), else to the board's console.
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | ✓ | ✓ | ✓ | ✓ |
 *
 * @module globals
 */

/**
 * Formatted output, one line per call (a subset of the Web / Node console).
 *
 * The arguments are separated by a space: a string is written as it is, any
 * other value is formatted like the QuickJS `qjs` console does. There is no
 * format string: `%s`, `%d`... are written as they are. The line is written
 * in one piece, so lines from different sources never mix.
 */
declare namespace console {
    /**
     * Writes the values on one line of the standard output.
     */
    function log(...values: unknown[]): void;

    /**
     * Same as {@link console.log}: one line of the standard output.
     */
    function info(...values: unknown[]): void;

    /**
     * Same as {@link console.log}: one line of the standard output.
     */
    function debug(...values: unknown[]): void;

    /**
     * Writes the values on one line of the standard ERROR output.
     */
    function warn(...values: unknown[]): void;

    /**
     * Same as {@link console.warn}: one line of the standard error output.
     */
    function error(...values: unknown[]): void;
}

/**
 * Calls `callback(...args)` once, after `delay` milliseconds.
 *
 * The delay is taken in whole milliseconds (the fraction is dropped); like
 * Node, a delay that is NaN, negative or above 2^31-1 ms (about 24.8 days)
 * becomes 1 ms. A pending timer keeps the program alive. An exception thrown
 * by the callback is reported as uncaught: on the PC the program then ends
 * with exit code 1, on a device it is printed and the program goes on.
 *
 * Unlike the Web, `callback` must be a function (a string of code is a
 * TypeError), and unlike Node the id is a plain number.
 *
 * @param callback the function to call.
 * @param delay milliseconds, default 0. Must be a number (TypeError otherwise).
 * @param args the arguments given to the callback.
 * @returns the id of the timer (an integer from 1 up), for {@link clearTimeout}.
 * @throws TypeError if `callback` is not a function or `delay` not a number.
 */
declare function setTimeout<A extends unknown[]>(callback: (...args: A) => void, delay?: number, ...args: A): number;

/**
 * Calls `callback(...args)` every `delay` milliseconds, until
 * {@link clearInterval}.
 *
 * Same rules as {@link setTimeout} for the delay (whole milliseconds; NaN,
 * negative or above 2^31-1 ms becomes 1 ms) and the callback. A running
 * interval keeps the program alive.
 *
 * @param callback the function to call.
 * @param delay milliseconds between two calls, default 0.
 * @param args the arguments given to the callback.
 * @returns the id of the timer (an integer from 1 up).
 * @throws TypeError if `callback` is not a function or `delay` not a number.
 * @example
 * const id = setInterval(() => console.log("tick"), 1000);
 * setTimeout(() => clearInterval(id), 5000);
 */
declare function setInterval<A extends unknown[]>(callback: (...args: A) => void, delay?: number, ...args: A): number;

/**
 * Cancels a timer made by {@link setTimeout} or {@link setInterval} (they
 * share one id space). Accepts anything, like the Web: `undefined`, a value
 * that is not a number or an unknown id does nothing.
 */
declare function clearTimeout(id?: unknown): void;

/**
 * Same as {@link clearTimeout}: cancels a timer made by {@link setInterval}
 * or {@link setTimeout}.
 */
declare function clearInterval(id?: unknown): void;

/**
 * Encodes strings into UTF-8 bytes (UTF-8 is the only encoding).
 */
declare class TextEncoder {
    /**
     * A new encoder. Arguments are ignored.
     */
    constructor();

    /**
     * Always `"utf-8"` (read-only).
     */
    readonly encoding: "utf-8";

    /**
     * The UTF-8 bytes of `text` in a new Uint8Array; an empty array without
     * argument. `encodeInto()` does not exist.
     *
     * @throws TypeError if `text` is given and is not a string (no implicit
     * conversion).
     */
    encode(text?: string): Uint8Array;
}

/**
 * Decodes UTF-8 bytes into a string (UTF-8 is the only encoding).
 *
 * Differences with the Web: a byte order mark at the start is KEPT (it
 * becomes U+FEFF), the `fatal` and `ignoreBOM` options are ignored, and
 * there is no streaming. Invalid sequences become U+FFFD.
 */
declare class TextDecoder {
    /**
     * A new decoder.
     *
     * @param label `"utf-8"` (default), `"utf8"` or `"unicode-1-1-utf-8"`,
     * compared without case; surrounding spaces are not accepted.
     * @param options accepted and ignored.
     * @throws RangeError for any other label; TypeError if `label` is given
     * and is not a string (`null` included).
     */
    constructor(label?: string, options?: { fatal?: boolean; ignoreBOM?: boolean });

    /**
     * Always `"utf-8"` (read-only).
     */
    readonly encoding: "utf-8";

    /**
     * The string whose UTF-8 encoding is `bytes`; `""` without argument. A
     * typed array other than a Uint8Array is decoded from its bytes.
     *
     * @throws TypeError if `bytes` is not a typed array or an ArrayBuffer (a
     * DataView or an array of numbers is refused).
     */
    decode(bytes?: ArrayBuffer | Int8Array | Uint8Array | Uint8ClampedArray | Int16Array | Uint16Array | Int32Array
        | Uint32Array | Float16Array | Float32Array | Float64Array | BigInt64Array | BigUint64Array): string;
}

/**
 * The running program: its platform, versions, arguments, environment,
 * current folder, output streams, and `exit()` (a subset of Node's
 * `process`, a global as in Node, not a module).
 *
 * On a device, `argv`, `env` and `cwd()` describe the program, not the
 * machine: see each member.
 */
declare namespace process {
    /**
     * The platform the program runs on: `"win32"`, `"darwin"` or `"linux"` on
     * the PC (the desktop agent included); `"esp32"` or `"rp2350"` on a
     * device. A digital twin reports its board's platform (`"esp32"` for the
     * boards of qjsx).
     */
    const platform: "win32" | "darwin" | "linux" | "esp32" | "rp2350";

    /**
     * Versions, as plain strings: `qjsx` (the runtime, e.g. `"0.1.0"`; on a
     * device the firmware's), `quickjs` (the engine, e.g. `"2026-06-04"`) and,
     * when the program runs from a package (`.qjspkg`, an executable, a
     * device), `build`: the build id of the package (a hash of its bytecode).
     * `build` is absent for `qjsx run script.js`.
     */
    const versions: { qjsx: string; quickjs: string; build?: string };

    /**
     * The command line, read once at start.
     *
     * - `qjsx run script.js a b`: `["<path of qjsx>", "script.js", "a", "b"]`
     *   (the options of `qjsx run`, before the script, are left out);
     * - an executable made by `qjsx build --executable`: `[exe, exe, ...args]`
     *   (`--qjsx-root-folder=...` is removed);
     * - a device (and the desktop agent, the twin): always
     *   `["qjsx", "<main module of the package>"]`.
     *
     * On Windows the paths use `/`.
     */
    const argv: string[];

    /**
     * Environment variables, read once at start: changing this object does not
     * change the real environment.
     *
     * - A program of the PC sees only the variables it was granted
     *   (`qjsx run -E` / `--allow-env[=names]`, or `permissions.env` of the
     *   `desktop` section of `qjsx.json`); without a grant it is `{}`.
     * - On a device it holds the strings of the `env` object of `qjsx.json`
     *   (copied into the package by `qjsx build`), never the environment of a
     *   PC; `{}` without one.
     */
    const env: { [name: string]: string | undefined };

    /**
     * Ends the PROGRAM with `code` (default 0), never the process that runs it:
     * the code running now is unwound at once, and no `catch` or `finally`
     * runs; no timer, callback or promise job runs any more. What was written
     * so far is flushed first.
     *
     * `qjsx run` and an executable return the code to the system. On a device
     * the agent reports the end to the PC (EXITED) and stays alive; the
     * program is not restarted.
     *
     * @param code an integer (TypeError otherwise, a string included).
     */
    function exit(code?: number): never;

    /**
     * The program's current folder, with `/` separators (also on Windows,
     * unlike Node). Relative paths of `sys/fs` resolve against it.
     *
     * - A program of the PC with a root folder (`rootFolder` of the `desktop`
     *   section of `qjsx.json`, `--qjsx-root-folder=...` or
     *   `QJSX_ROOT_FOLDER`): that folder. Without one: the system's current
     *   folder.
     * - A device: its data folder (the desktop agent and the twin:
     *   `<storage>/data`); `"/"` on a device without file system (the ESP32
     *   and RP2350 boards today).
     *
     * There is no `chdir()`.
     *
     * @throws Error with a `code` if the system's current folder cannot be read.
     */
    function cwd(): string;

    /**
     * The standard output.
     */
    const stdout: {
        /**
         * Writes `text` as it is (no newline added). Always returns `true`.
         *
         * @throws TypeError if `text` is not a string (bytes are refused).
         */
        write(text: string): true;
    };

    /**
     * The standard error output.
     */
    const stderr: {
        /**
         * Writes `text` as it is (no newline added). Always returns `true`.
         *
         * @throws TypeError if `text` is not a string (bytes are refused).
         */
        write(text: string): true;
    };
}

/**
 * `import.meta` of a qjsx module.
 */
interface ImportMeta {
    /**
     * The module's name: a `file://` URL when the name is an absolute path,
     * else the name as the loader knows it (a relative path for
     * `qjsx run script.js`, the module name inside a package).
     */
    url: string;

    /**
     * True for the main module of the program (the one `qjsx run` or the
     * package starts), false for the modules it imports.
     */
    main: boolean;
}
