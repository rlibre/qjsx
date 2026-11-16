/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/**
 * The board the program runs on: the unique id of its chip, its MAC
 * addresses, its restart.
 *
 * @example
 * import { id, mac, restart } from "device/board";
 *
 * const serial = id().toHex().toUpperCase();          // "1020BA69D7D0" on an ESP32
 * const address = [...mac("wifi")].map(byte => byte.toString(16).padStart(2, "0")).join(":");
 * restart();                                         // never returns
 *
 * @compatibility
 * | PC | ESP32 | RP2350 | Twin |
 * |---|---|---|---|
 * | — ¹ | ✓ | — | ✓ ² |
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
}
