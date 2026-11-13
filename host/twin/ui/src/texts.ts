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
        used_pins_only_description: "The table of the pins, and the list of the analyzer, show only the pins the program configured, a part uses, or the page forces.",
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
        // the tooltips are HTML (x4js): <br> breaks a line
        scope_run_tooltip: "Pressed: the view follows the twin (Shift + wheel does nothing)<br>Released: the view freezes (on the last recording, if any), and Shift + wheel moves it in time",
        scope_per_div:  "/div",                 // after a time base: "1 ms/div"
        scope_add:      "add a pin...",
        scope_remove:   "Remove",               // menu of the button of a pin
        scope_color:    "Color",                // its sub menu of the colors
        scope_switch_pin: "Switch pin",         // its sub menu of the other pins
        scope_channel_tooltip: "The color of its trace, another pin in its place, or remove it",
        scope_colors:   ["Yellow", "Cyan", "Magenta", "Green", "Orange", "Violet", "Blue", "Red"],   // --scope-color-<n>
        scope_clear:    "Clear",
        scope_record:   "Record",
        scope_record_tooltip: "On: record from now on<br>Off: Run stops too, the view freezes on the recording; a right click on the traces saves it as a file",
        scope_record_full: "the recording is full (1,000,000 changes): stopped",
        scope_save:     "Save the recording (VCD)",   // context menu of the traces: the pins shown, for PulseView, GTKWave
        scope_save_cut: "1,000,000 changes at most: the file stops at",   // then a time: "... at 12 ms"
        scope_rmt_bytes: "bytes",               // a band of an RMT send: "RMT · 3 bytes"
        scope_rmt_pulses: "pulses",             // "RMT · 40 pulses"
        scope_empty:    "Add a pin to see its level over time",
        scope_traces_tooltip: "Wheel: scroll the pins<br>Shift + wheel: move in time (frozen view)<br>Ctrl + wheel: zoom<br>Right click: save, clear",
        scope_modes:    ["Roll", "Normal", "Single"],   // the list of the modes (ScopeView: roll, normal, single)
        scope_mode_tooltip: "Roll: the view follows the twin<br>Normal: it stays on the last trigger<br>Single: it freezes on the first trigger after Run",
        scope_trigger_tooltip: "The pin whose edges trigger the view (Normal, Single)",
        scope_edges:    ["Rising", "Falling", "Both edges"],   // the list of the edges (rising, falling, both)
        scope_edge_tooltip: "The edge that triggers",
        scope_waiting:  "waiting for a trigger",
        i2c:            "I2C",                  // title of an I2C bus section: "I2C 0"
        i2c_closed:     "closed",
        i2c_clear:      "Clear the transfers",  // menu of a bus: empties its trace
        i2c_menu_tooltip: "This bus: its trace",
        i2c_probe:      "probe",                // a transfer of nothing
        uart:           "UART",                 // title of the section of a UART: "UART 1"
        uart_links_tooltip: "This UART: its TCP port, a serial port of the PC, how its traffic is shown",
        uart_link_tcp:  "TCP",                  // the sub menu of the TCP link of a UART
        uart_closed:    "closed",
        uart_hex_view:  "Show in hexadecimal",  // menu of a UART: the traffic in hexadecimal, not text
        uart_clear:     "Clear the traffic",
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
        uart_from_serial: "serial",
        serial_port:    "PC port",              // the wiring of a UART to a serial port of the PC
        serial_choose:  "choose a port...",
        serial_none:    "no port found",
        serial_connect: "connect",
        serial_connect_tooltip: "Wire this UART to a serial port of the PC: what the program writes goes out on it, what it receives goes to the program",
        serial_real_speed: "real speed",
        serial_real_speed_tooltip: "The program's writes take the time the bytes take on the line; unchecked, they return at once",
        serial_off:     "not connected",
        serial_waiting: "connects when the program opens the UART",
        serial_connected: "connected",
        serial_no_port: "choose a port first",
        serial_fixed:   "set by the command line or qjsx.json, not saved",
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
