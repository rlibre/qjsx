/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What the section of an SPI bus shows: the trace of the transactions of the
// program on it (tracepanel.ts), one line each: when, the device (its chip
// select, clock, mode), then each segment with its bytes in hexadecimal
// (the first 16; the size after a longer one):
//   W 2A        a write (C 2A / D 2A with the level of the DC pin, when the
//               device has one: C command, D data)
//   R FF FF     a read
//   X 01 02 → FF FF   a transfer (full duplex): sent, then received
//   wait 10 µs  a delay, the chip select held
// and "no part" (nothing answered: 0xFF read) or "TIMEOUT". The section is
// shown while the program has the bus open.
import { BoxProps } from "x4js";
import { frequencyText } from "./frequency";
import { SpiBusState, SpiRecord, SpiSegment } from "./messages";
import { tx } from "./texts";
import { asHex, timeText, TracePanel } from "./tracepanel";

/**
 * The title line of the section of a bus: "closed", or "sclk 12 · mosi 11 ·
 * miso 13 · 2 devices" (a data pin not wired is left out).
 */
export function spiSummary(state: SpiBusState) {
    // closed: only that
    if (!state.open) {
        return tx.twin.spi_closed;
    }

    // its pins (those wired), then its devices
    const pins = [`sclk ${state.sclk}`];
    if (state.mosi !== null) {
        pins.push(`mosi ${state.mosi}`);
    }

    if (state.miso !== null) {
        pins.push(`miso ${state.miso}`);
    }

    const count = state.devices.length;
    const devices = `${count} ${count === 1 ? tx.twin.spi_device : tx.twin.spi_devices}`;
    return [...pins, devices].join(" · ");
}

/**
 * The letter of a write: W, or the level of the DC pin when the device has
 * one ('hasDc'): C command (0), D data (1).
 */
function writeLetter(segment: SpiSegment, hasDc: boolean) {
    if (!hasDc) {
        return "W";
    }

    return segment.dc === 0 ? "C" : "D";
}

/**
 * The text of a segment ('hasDc': the device has a DC pin).
 */
function segmentText(segment: SpiSegment, hasDc: boolean) {
    switch (segment.kind) {
        case "write": {
            return `${writeLetter(segment, hasDc)} ${asHex(segment.written, segment.size)}`;
        }
        case "read": {
            return `R ${asHex(segment.read, segment.size)}`;
        }
        case "transfer": {
            return `X ${asHex(segment.written, segment.size)} → ${asHex(segment.read, segment.size)}`;
        }
        default: {
            return `${tx.twin.spi_wait} ${segment.delayUs} µs`;
        }
    }
}

/**
 * The text of a transaction: "12.345 s  cs 10  26.7 MHz  mode 3  C 2A  D 00
 * 00 00 EF  R FF FF", then how it went when no part answered.
 */
function recordText(record: SpiRecord) {
    // when, which device: its chip select (or the program's), clock, mode
    const cs = record.cs === null ? tx.twin.spi_cs_program : `cs ${record.cs}`;
    const bitOrder = record.bitOrder === "lsb" ? "  lsb" : "";
    const parts = [timeText(record.time), cs, frequencyText(record.frequency), `mode ${record.mode}${bitOrder}`];

    // its segments, and how many more the trace did not keep
    const hasDc = record.dc !== null;
    parts.push(...record.segments.map(segment => segmentText(segment, hasDc)));
    const notKept = record.segmentCount - record.segments.length;
    if (notKept > 0) {
        parts.push(`… +${notKept}`);
    }

    // how it went, when no part answered
    if (record.outcome === "nobody") {
        parts.push(tx.twin.spi_no_part);
    } else if (record.outcome === "timeout") {
        parts.push("TIMEOUT");
    }

    return parts.join("  ");
}

export class SpiPanel extends TracePanel {
    /**
     * An empty trace, and the settings button its section shows.
     */
    constructor(props: BoxProps) {
        super(props, tx.twin.spi_menu_tooltip, tx.twin.spi_clear);
    }

    /**
     * Adds transactions to the trace (one that timed out has that class).
     */
    addRecords(records: SpiRecord[]) {
        this.addLines(records.map(record => ({ text: recordText(record), cls: record.outcome })));
    }
}
