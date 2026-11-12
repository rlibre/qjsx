/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The texts of the page, in English (as every message of qjsx). Declared
// with addTranslation and selected before any component is built (main.ts);
// the components read them through 'tx', the typed view of x4's _tr.
import { _tr, addTranslation } from "x4js";

const twin_en = {
    twin: {
        title:          "qjsx twin",            // header, and suffix of the window title
        connecting:     "connecting...",
        connected:      "connected",
        disconnected:   "disconnected: waiting for the twin...",
        board:          "Board",                // title of the board panel
        buttons:        "Buttons",              // the section of the push buttons
        pins:           "Pins",                 // title of the pins panel, and section of the settings
        reset_layout:   "Reset the layout",     // context menu of the panels
        settings:       "Settings",
        display:        "Display",              // section of the settings
        theme:          "Theme",
        theme_description: "The colors of the page.",
        theme_system:   "As the system",
        theme_light:    "Light",
        theme_dark:     "Dark",
        used_pins_only: "Used pins only",
        used_pins_only_description: "The table shows only the pins the program configured, a part uses, or the page forces.",
        gpio:           "GPIO",                 // column titles of the pins
        mode:           "mode",
        pull:           "pull",
        part:           "part",
        force:          "force",
        force_on:       "on",                   // force buttons: the pin at 1, at 0, let go
        force_off:      "off",
        free:           "free",
        free_tooltip:   "Not forced by the page: the level comes from the program, then from a part, then from the pulls",
        conflict_tooltip: "The program and a part drive different levels: a short on a real board",
        console:        "Console",              // title of the panel of the lines of the parts
        console_clear:  "Clear",
        scope:          "Analyzer",             // title of the panel of the levels of the pins over time
        scope_run:      "Run",
        scope_stop:     "Stop",
        scope_per_div:  "/div",                 // after a time base: "1 ms/div"
        scope_add:      "add a pin...",
        scope_remove:   "Remove",               // menu of the button of a pin
        scope_channel_tooltip: "The color of its trace, or remove it",
        scope_colors:   ["Yellow", "Cyan", "Magenta", "Green", "Orange", "Violet", "Blue", "Red"],   // --scope-color-<n>
        scope_clear:    "Clear",
        scope_empty:    "Add a pin to see its level over time",
        i2c:            "I2C",                  // title of an I2C bus section: "I2C 0"
        i2c_closed:     "closed",
        i2c_trace:      "Transfers",
        i2c_probe:      "probe",                // a transfer of nothing
        uart:           "UART",                 // title of the section of a UART: "UART 1"
        uart_log:       "Traffic",              // above the log of a UART
        uart_closed:    "closed",
        uart_hex_view:  "hex",                  // show the traffic in hexadecimal
        uart_hex_view_tooltip: "Show the bytes in hexadecimal instead of text",
        uart_clear:     "Clear",
        uart_send:      "Send",
        uart_hex_input: "hex",                  // what is typed is hexadecimal
        uart_hex_input_tooltip: "Type bytes in hexadecimal: 02 A0 FF",
        uart_text_placeholder: "text for the program",
        uart_hex_placeholder:  "bytes for the program: 02 A0 FF",
        uart_bad_hex:   "not hexadecimal bytes",
        uart_ending_none: "no line ending",      // line endings added to the text sent
        uart_ending_lf: "\\n",
        uart_ending_crlf: "\\r\\n",
        uart_from_page: "page",                 // where a received record comes from
        uart_from_tcp:  "tcp",
        tcp_port:       "TCP port",             // the TCP link of a UART or of a USB function
        tcp_listen:     "listen",
        tcp_listen_tooltip: "Listen on this TCP port: a program of the PC connects to it and plays the other end",
        tcp_off:        "not listening",
        tcp_waiting:    "no client",
        tcp_client:     "client connected",
        tcp_when_started: "listens once the program starts the device",
        tcp_no_port:    "choose a port first",
        usb_title:      "USB",                  // title of the section of the USB device
        usb_stopped:    "not started by the program",
        usb_started:    "started",
        usb_serial:     "serial",               // "serial 0": a serial function
        usb_device_stopped: "unused: the device is not started",
        usb_not_declared: "unused: the program did not declare it",
        usb_no_host:    "used, waiting for a host",
        usb_host:       "used, host connected (DTR, RTS)",
    },
};

/**
 * Declares the texts of the page (before selectLanguage).
 */
export function addTexts() {
    addTranslation("en", twin_en);
}

export const tx = _tr as typeof _tr & typeof twin_en;
