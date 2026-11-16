/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * The board the program runs on: the unique id of its chip, its MAC
 * addresses, its restart, its memory and the load of the program.
 *
 * @example
 * import { id, mac, restart, memory, load } from "device/board";
 *
 * const serial = id().toHex().toUpperCase();          // "1020BA69D7D0" on an ESP32
 * const address = [...mac("wifi")].map(byte => byte.toString(16).padStart(2, "0")).join(":");
 * console.log(`heap ${memory().jsHeap} bytes, load ${Math.round(load().js * 100)} %`);
 * restart();                                         // never returns
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | i.MX RT1062 | Twin |
 * |---|---|---|---|---|
 * | — ¹ | ✓ | — | ✓ | ✓ ² |
 *
 * ¹ Not in `qjsx run` nor in a program of the PC; the desktop agent (`qjsx agent`) has it.
 *
 * ² The desktop agent and the twin keep a base MAC in their storage (`board-id`), drawn at
 * random the first time: the same installation always gives the same id.
 *
 * @module device/board
 */
declare module "device/board" {
    /** The MAC addresses of a chip, all derived from its base MAC. */
    export type MacKind = "base" | "wifi" | "ap" | "bluetooth" | "ethernet";

    /**
     * The unique id of the chip, in a new Uint8Array: on an ESP32 its base MAC
     * (6 bytes, from the eFuses), on the desktop agent and the twin the 6 bytes
     * kept in their storage. Format it as needed, e.g. `id().toHex()`.
     */
    export function id(): Uint8Array;

    /**
     * A MAC address of the chip, in a new 6-byte Uint8Array. ESP32: as ESP-IDF
     * derives them from the base MAC (WiFi station = base, access point,
     * Bluetooth, Ethernet next); the desktop agent and the twin do the same
     * (base, +1, +2, +3).
     *
     * @param kind which address. Default `"base"`.
     * @throws Error when the chip has no address of that kind; TypeError for
     *   an unknown kind.
     */
    export function mac(kind?: MacKind): Uint8Array;

    /**
     * Restarts the device, like a power on, and never returns.
     *
     * - First, what the program wrote is really sent, for at most 500 ms in
     *   all: the console and the agent's link, the UARTs open by
     *   `device/serial`, the serial functions of `device/usb` (an answer to a
     *   host, then a restart, arrives).
     * - Nothing more of the program runs: no `catch`, `finally`, promise or
     *   timer, as with `process.exit()`.
     * - On a board the chip restarts and the program installed in flash starts
     *   at boot; a program loaded in RAM (`qjsx deploy --ram`) is lost. The
     *   desktop agent and the twin do what `qjsx device reset` does there: the
     *   installed program runs again.
     *
     * @throws Error outside a device (`qjsx run`).
     */
    export function restart(): never;

    /**
     * What {@link memory} reports, in bytes; null for what the board cannot
     * tell.
     */
    export interface MemoryStatus {
        /** The JS heap of the program now. */
        jsHeap: number | null;

        /** The most it used since the program started. */
        jsHeapPeak: number | null;

        /**
         * The region reserved for the JS heap on a board without PSRAM (the
         * most it can hold); null when the heap has no fixed size (PSRAM, the
         * desktop agent and the twin).
         */
        jsHeapSize: number | null;

        /** Free internal RAM, left to the firmware; null on the desktop agent and the twin. */
        ramFree: number | null;

        /** The largest free block of internal RAM. */
        ramLargest: number | null;

        /** The lowest free internal RAM since the boot. */
        ramLowest: number | null;

        /** Free PSRAM; null without PSRAM. */
        psramFree: number | null;
    }

    /**
     * The memory of the board and the JS heap of the program, as `qjsx device
     * info` shows them. On the desktop agent and the twin the JS heap is
     * measured on the PC: values and pointers take twice the room there.
     */
    export function memory(): MemoryStatus;

    /**
     * What {@link load} reports: how much the JS thread of the program works,
     * as opposed to waiting for an event or a timer in its event loop.
     */
    export interface LoadStatus {
        /**
         * The share of the last second the JS thread worked, 0 to 1 (less
         * than a second since the start: the share of that time). The call
         * itself counts as work.
         */
        js: number;

        /** The most of `js` over a full second since the program started. */
        jsPeak: number;

        /** Milliseconds the JS thread worked since the program started. */
        busy: number;

        /**
         * Milliseconds since the program started. Between two calls,
         * `(busy2 - busy1) / (elapsed2 - elapsed1)` is the load over that time.
         */
        elapsed: number;
    }

    /**
     * The load of the JS thread of the program. Work is everything but the
     * waits of the event loop: JS code, native calls that block it (a
     * synchronous I2C transfer), and a pause in the debugger.
     *
     * @example
     * const before = load();
     * // ... later
     * const after = load();
     * const average = (after.busy - before.busy) / (after.elapsed - before.elapsed);
     */
    export function load(): LoadStatus;
}
