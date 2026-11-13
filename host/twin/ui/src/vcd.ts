/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A recording of the analyzer as a VCD file (Value Change Dump, IEEE 1364):
// the format of logic analyzers, read by PulseView and GTKWave. One variable
// per pin: a wire of 1 bit for a logic pin ('x' while it was busy: it
// changed faster than the twin reported), a real for an analog one (volts).
// Times in ns from the start of the recording (user, 2026-11-13: the edges
// of a fast PWM fall between two µs).
//
// A PWM segment is written as its edges, computed from its frequency and
// duty cycle, an RMT send as its pulses (scopetrace.ts). A fast PWM makes
// many: the file stops at MAX_CHANGES changes, and says where (cutAt).
//
// A recording may hold a million changes: the channels are merged in time
// order with a cursor each (no object per change), and the text is given in
// parts (a Blob takes them as they are).
import {
    firstStateAfter, MAX_CHANGES, PinTrace, pwmChangeLevel, pwmChangeTime, pwmFirstChangeAfter, pwmLevelAt,
    pwmToggles, Recording, rmtPulseCount, rmtPulseLevel, rmtPulseTicks,
} from "./scopetrace";

// A pin written to the file.
export interface VcdChannel {
    pin:    number;
    name:   string;                      // "GPIO5"
    analog: boolean;                     // written as its voltage
    vref:   number;                      // the voltage of its level 1, in volts (a PWM on an analog pin)
}

// What recordingToVcd() gives.
export interface Vcd {
    parts: string[];                     // the text of the file
    cutAt: number | null;                // µs from the start where it stopped (MAX_CHANGES); null: complete
}

// Lines gathered into one part of the text.
const LINES_PER_PART = 4096;

// Where the merge is in one channel: its next state, its next busy time,
// its PWM segment and the next change in it, its RMT send and the next
// pulse in it.
interface Cursor {
    trace:     PinTrace;
    analog:    boolean;
    vref:      number;                   // volts
    id:        string;                   // its identifier in the file
    state:     number;                   // index of its next state
    end:       number;                   // index after its last state in the recording
    busy:      number;                   // index of its next busy time (logic pins)
    segment:   number;                   // index of its PWM segment (the current one, else the next)
    change:    number;                   // number of the next change in that segment; -1: not entered yet
    train:     number;                   // index of its RMT send (the current one, else the next)
    pulse:     number;                   // the next pulse of that send; -1: not entered yet; the pulse count: its end
    pulseTime: number;                   // µs: when that pulse starts
}

/**
 * The identifier of the n-th variable: a printable character from '!'.
 */
function identifier(index: number) {
    return String.fromCharCode(33 + index);
}

/**
 * The time of the next state of a cursor, µs from the start (Infinity when
 * it has none left).
 */
function nextStateTime(cursor: Cursor, recording: Recording): number {
    if (cursor.state >= cursor.end) {
        return Infinity;
    }
    return Math.max(0, cursor.trace.states.time(cursor.state) - recording.start);
}

/**
 * The time the next busy time of a cursor starts, µs from the start
 * (Infinity when it has none left, or for an analog pin).
 */
function nextBusyTime(cursor: Cursor, recording: Recording): number {
    const busy = cursor.trace.busy;
    if (cursor.analog || cursor.busy >= busy.length) {
        return Infinity;
    }
    return busy[cursor.busy].from - recording.start;
}

/**
 * The time of the next PWM change of a cursor, µs from the start (Infinity
 * when none is left): entering its segment (its level there: the start of
 * the segment, or of the recording), then each change of level in it. A
 * segment whose changes are over is left first.
 */
function nextPwmTime(cursor: Cursor, recording: Recording): number {
    const segments = cursor.trace.pwm;
    while (cursor.segment < segments.length) {
        // entering it
        const segment = segments[cursor.segment];
        if (cursor.change < 0) {
            return Math.max(segment.from, recording.start) - recording.start;
        }

        // its next change, before its end
        const time = pwmChangeTime(segment, cursor.change);
        if (pwmToggles(segment) && time < segment.to) {
            return time - recording.start;
        }

        // over: the next segment
        cursor.segment++;
        cursor.change = -1;
    }
    return Infinity;
}

/**
 * The value of a level in the file, for a cursor: a bit, or the voltage of
 * an analog pin.
 */
function levelLine(cursor: Cursor, level: number): string {
    if (cursor.analog) {
        return `r${(level !== 0 ? cursor.vref : 0).toFixed(3)} ${cursor.id}`;
    }
    return `${level !== 0 ? 1 : 0}${cursor.id}`;
}

/**
 * The line of the next PWM change of a cursor (nextPwmTime said when), the
 * cursor moved past it.
 */
function takePwmChange(cursor: Cursor, recording: Recording): string {
    const segment = cursor.trace.pwm[cursor.segment];

    // entering the segment: its level there, then its changes after it
    if (cursor.change < 0) {
        const entry = Math.max(segment.from, recording.start);
        cursor.change = pwmFirstChangeAfter(segment, entry);
        return levelLine(cursor, pwmLevelAt(segment, entry));
    }

    // a change of level
    const level = pwmChangeLevel(segment, cursor.change);
    cursor.change++;
    return levelLine(cursor, level);
}

/**
 * The time of the next RMT change of a cursor, µs from the start (Infinity
 * when none is left): entering its send (its level there: the start of the
 * send, or of the recording), the start of each pulse after that, then the
 * end of the send (the pin back to its state). A send that is over is left
 * first.
 */
function nextRmtTime(cursor: Cursor, recording: Recording): number {
    const trains = cursor.trace.rmt;
    while (cursor.train < trains.length) {
        const train = trains[cursor.train];
        const count = rmtPulseCount(train);
        if (cursor.pulse < 0) {
            return Math.max(train.from, recording.start) - recording.start;
        }
        if (cursor.pulse < count) {
            return cursor.pulseTime - recording.start;
        }
        if (cursor.pulse === count) {
            return train.to - recording.start;
        }

        // over: the next send
        cursor.train++;
        cursor.pulse = -1;
    }
    return Infinity;
}

/**
 * The line of the state of a cursor at 'time' (the last one at or before it).
 */
function stateLine(cursor: Cursor, time: number): string {
    const states = cursor.trace.states;
    const index = Math.max(0, firstStateAfter(states, time) - 1);
    if (cursor.analog) {
        return `r${(states.millivolts(index) / 1000).toFixed(3)} ${cursor.id}`;
    }
    return `${states.level(index) !== 0 ? 1 : 0}${cursor.id}`;
}

/**
 * The line of the next RMT change of a cursor (nextRmtTime said when), the
 * cursor moved past it.
 */
function takeRmtChange(cursor: Cursor, recording: Recording): string {
    const train = cursor.trace.rmt[cursor.train];
    const count = rmtPulseCount(train);

    // entering the send: the pulse at that instant, the next one after it
    if (cursor.pulse < 0) {
        const entry = Math.max(train.from, recording.start);
        let index = 0;
        let time = train.from;
        while (index < count && time + rmtPulseTicks(train, index) * train.tickUs <= entry) {
            time += rmtPulseTicks(train, index) * train.tickUs;
            index++;
        }
        if (index === count) {
            cursor.pulse = count + 1;
            return stateLine(cursor, train.to);
        }
        cursor.pulse = index + 1;
        cursor.pulseTime = time + rmtPulseTicks(train, index) * train.tickUs;
        return levelLine(cursor, rmtPulseLevel(train, index));
    }

    // a pulse
    if (cursor.pulse < count) {
        const level = rmtPulseLevel(train, cursor.pulse);
        cursor.pulseTime += rmtPulseTicks(train, cursor.pulse) * train.tickUs;
        cursor.pulse++;
        return levelLine(cursor, level);
    }

    // its end: the pin back to its state
    cursor.pulse++;
    return stateLine(cursor, train.to);
}

/**
 * The line of the next state of a cursor, or of its busy time when it
 * starts before (unknown: 'x'), the cursor moved past it.
 */
function takeChange(cursor: Cursor, recording: Recording): string {
    // a busy time starting before the next state: unknown
    if (nextBusyTime(cursor, recording) < nextStateTime(cursor, recording)) {
        cursor.busy++;
        return `x${cursor.id}`;
    }

    // the next state
    const states = cursor.trace.states;
    const index = cursor.state++;
    if (cursor.analog) {
        return `r${(states.millivolts(index) / 1000).toFixed(3)} ${cursor.id}`;
    }
    return `${states.level(index) !== 0 ? 1 : 0}${cursor.id}`;
}

/**
 * The VCD text of 'recording' for 'channels', in parts:
 * 1. the header: the time unit, one variable per pin;
 * 2. a cursor per pin: from its state at the start (the last one before it)
 *    to its last change in the recording, its busy times, PWM segments and
 *    RMT sends within it;
 * 3. the changes of every pin in time order, a "#time" line before each
 *    group of changes at the same time; at the same time, a state, then a
 *    PWM change, then an RMT one (the last wins: VCD keeps the last value);
 *    up to MAX_CHANGES.
 */
export function recordingToVcd(recording: Recording, channels: VcdChannel[]): Vcd {
    const parts: string[] = [];
    let lines: string[] = [];

    // keeps the lines gathered as one part
    const flush = () => {
        parts.push(`${lines.join("\n")}\n`);
        lines = [];
    };

    // 1. the header
    lines.push(`$date ${new Date().toISOString()} $end`);
    lines.push("$version qjsx twin analyzer $end");
    lines.push("$timescale 1ns $end");
    lines.push("$scope module twin $end");
    channels.forEach((channel, index) => {
        const kind = channel.analog ? "real 64" : "wire 1";
        lines.push(`$var ${kind} ${identifier(index)} ${channel.name} $end`);
    });
    lines.push("$upscope $end");
    lines.push("$enddefinitions $end");

    // 2. the cursors
    const cursors: Cursor[] = [];
    channels.forEach((channel, index) => {
        const trace = recording.traces[channel.pin];
        if (trace === undefined) {
            return;
        }
        const states = trace.states;
        const busy = trace.busy.findIndex(interval => interval.from >= recording.start);
        cursors.push({
            trace,
            analog: channel.analog,
            vref: channel.vref,
            id: identifier(index),
            state: Math.max(0, firstStateAfter(states, recording.start) - 1),
            end: firstStateAfter(states, recording.end),
            busy: busy < 0 ? trace.busy.length : busy,
            segment: 0,
            change: -1,
            train: 0,
            pulse: -1,
            pulseTime: 0,
        });
    });

    // 3. the changes, the soonest cursor first each time
    const length = recording.end - recording.start;
    let current = -1;
    let written = 0;
    let cutAt: number | null = null;
    for (;;) {
        // the soonest change of every cursor: a state (or busy time), a PWM
        // change or an RMT one (in that order at the same time)
        let soonest: Cursor = null;
        let soonestTime = Infinity;
        let soonestKind: "state" | "pwm" | "rmt" = "state";
        for (const cursor of cursors) {
            const stateTime = Math.min(nextStateTime(cursor, recording), nextBusyTime(cursor, recording));
            const pwmTime = nextPwmTime(cursor, recording);
            const rmtTime = nextRmtTime(cursor, recording);
            const time = Math.min(stateTime, pwmTime, rmtTime);
            if (time < soonestTime) {
                soonest = cursor;
                soonestTime = time;
                soonestKind = time === stateTime ? "state" : (time === pwmTime ? "pwm" : "rmt");
            }
        }
        if (soonest === null || soonestTime > length) {
            break;
        }

        // the cap: the file stops there
        if (written >= MAX_CHANGES) {
            cutAt = soonestTime;
            break;
        }

        // its time, its line
        const time = Math.round(soonestTime * 1000);
        if (time !== current) {
            lines.push(`#${time}`);
            current = time;
        }
        if (soonestKind === "state") {
            lines.push(takeChange(soonest, recording));
        } else if (soonestKind === "pwm") {
            lines.push(takePwmChange(soonest, recording));
        } else {
            lines.push(takeRmtChange(soonest, recording));
        }
        written++;
        if (lines.length >= LINES_PER_PART) {
            flush();
        }
    }

    lines.push(`#${Math.round((cutAt ?? length) * 1000)}`);
    flush();
    return { parts, cutAt };
}
