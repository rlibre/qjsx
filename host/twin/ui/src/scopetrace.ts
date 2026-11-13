/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What the analyzer keeps of a pin (scopeview.ts draws it, vcd.ts writes
// it): its states over time, the times it changed faster than the twin
// reported, and the times a PWM output drove it. Times in µs since the twin
// started.
//
// The states are kept compact (user: a recording may hold a million of
// them): one typed array per field (11 bytes a state: time, level,
// millivolts) instead of an object each (about 50 bytes).
//
// A PWM output is kept as segments (its frequency and duty cycle from one
// time to another), not as edges: the signal is computed from them where
// it is needed (drawn, written to a file), at any frequency.

// The changes a recording keeps, at most (every pin together); the edges a
// VCD file of it holds, at most, too.
export const MAX_CHANGES = 1000000;

// The first capacity of a list of states; it doubles when full.
const FIRST_CAPACITY = 64;

// The states of a pin, oldest first, in columns. Index 0 is the oldest kept;
// the oldest can be dropped (dropOldest) without moving the others at once.
export class StateList {
    private _times = new Float64Array(FIRST_CAPACITY);       // µs since the twin started
    private _levels = new Uint8Array(FIRST_CAPACITY);        // 0 or 1
    private _millivolts = new Int16Array(FIRST_CAPACITY);   // the voltage after the change
    private _first = 0;                                      // index in the arrays of the oldest kept
    private _end = 0;                                        // index in the arrays after the newest

    /**
     * The states kept.
     */
    get length(): number {
        return this._end - this._first;
    }

    /**
     * The time of state 'index' (0: the oldest kept), in µs.
     */
    time(index: number): number {
        return this._times[this._first + index];
    }

    /**
     * The level of state 'index'.
     */
    level(index: number): number {
        return this._levels[this._first + index];
    }

    /**
     * The voltage of state 'index', in mV.
     */
    millivolts(index: number): number {
        return this._millivolts[this._first + index];
    }

    /**
     * Adds a state, the newest; the arrays grow (twice as large, the
     * dropped ones left out) when full.
     */
    push(time: number, level: number, millivolts: number) {
        if (this._end === this._times.length) {
            this._grow();
        }
        this._times[this._end] = time;
        this._levels[this._end] = level;
        this._millivolts[this._end] = millivolts;
        this._end++;
    }

    /**
     * Drops the 'count' oldest states.
     */
    dropOldest(count: number) {
        this._first = Math.min(this._end, this._first + count);
    }

    /**
     * A copy of the states from 'from' to 'to' (indexes, 'to' excluded).
     */
    slice(from: number, to: number): StateList {
        const copy = new StateList();
        for (let index = from; index < to; index++) {
            copy.push(this.time(index), this.level(index), this.millivolts(index));
        }
        return copy;
    }

    /**
     * New arrays: room for twice the states kept (at least the first
     * capacity), those kept copied to their start.
     */
    private _grow() {
        const kept = this.length;
        const capacity = Math.max(FIRST_CAPACITY, kept * 2);
        const times = new Float64Array(capacity);
        const levels = new Uint8Array(capacity);
        const millivolts = new Int16Array(capacity);
        times.set(this._times.subarray(this._first, this._end));
        levels.set(this._levels.subarray(this._first, this._end));
        millivolts.set(this._millivolts.subarray(this._first, this._end));
        this._times = times;
        this._levels = levels;
        this._millivolts = millivolts;
        this._first = 0;
        this._end = kept;
    }
}

// A time when a pin changed faster than the twin reported (few of them: a
// plain object each).
export interface BusyInterval {
    from: number;                        // µs
    to:   number;
}

// A time a PWM output drove a pin (few of them: a plain object each). Each
// period starts with its active share: high, low when inverted. While it
// runs, the pin has no state of its own (the twin records none).
export interface PwmSegment {
    from:      number;                   // µs: the start of its first period
    to:        number;                   // µs; Infinity while it runs
    frequency: number;                   // Hz
    duty:      number;                   // 0 to 1, the share of a period it is active
    invert:    boolean;                  // active low
}

// An RMT send on a pin (few of them: a plain object each), kept as the hal
// got it: pulses, or bytes whose bits are pulses (most significant first,
// each bit high then low, then the reset low). Its pulses are read by
// number (rmtPulseCount, rmtPulseTicks, rmtPulseLevel), never stored one by
// one: a strip of LEDs sends thousands of them. After it, the pin is low.
export interface RmtTrain {
    from:   number;                      // µs: its start
    to:     number;                      // µs: its end
    tickUs: number;                      // the duration of a tick, µs
    pulses: number[];                    // a send of pulses: [ticks, level, ...]; empty for bytes
    bytes:  number[];                    // a send of bytes; empty for pulses
    bit0:   number[];                    // bytes: [high, low] ticks of a 0
    bit1:   number[];                    // bytes: [high, low] ticks of a 1
    reset:  number;                      // bytes: ticks low after them
}

// What a pin did: its states, its busy times, its PWM segments and RMT sends
// (each in time order, none overlapping).
export interface PinTrace {
    states: StateList;
    busy:   BusyInterval[];
    pwm:    PwmSegment[];
    rmt:    RmtTrain[];
}

/**
 * The pulses of an RMT send: those sent, or two per bit of its bytes and
 * the reset.
 */
export function rmtPulseCount(train: RmtTrain): number {
    if (train.bytes.length === 0) {
        return train.pulses.length / 2;
    }
    return train.bytes.length * 16 + (train.reset > 0 ? 1 : 0);
}

/**
 * The [high, low] ticks of bit 'bit' of the bytes of a send (bit 0: the most
 * significant of the first byte).
 */
function bitTiming(train: RmtTrain, bit: number): number[] {
    const byte = train.bytes[bit >> 3];
    const value = (byte >> (7 - (bit & 7))) & 1;
    return value !== 0 ? train.bit1 : train.bit0;
}

/**
 * The duration of pulse 'index' of a send, in ticks.
 */
export function rmtPulseTicks(train: RmtTrain, index: number): number {
    if (train.bytes.length === 0) {
        return train.pulses[index * 2];
    }

    // the reset after the bytes, else the high or low half of a bit
    if (index >= train.bytes.length * 16) {
        return train.reset;
    }
    return bitTiming(train, index >> 1)[index & 1];
}

/**
 * The level of pulse 'index' of a send: as sent, or high then low for each
 * bit, low for the reset.
 */
export function rmtPulseLevel(train: RmtTrain, index: number): number {
    if (train.bytes.length === 0) {
        return train.pulses[index * 2 + 1];
    }
    if (index >= train.bytes.length * 16) {
        return 0;
    }
    return (index & 1) === 0 ? 1 : 0;
}

/**
 * The total duration of a send, in ticks.
 */
export function rmtTotalTicks(train: RmtTrain): number {
    let ticks = 0;
    const count = rmtPulseCount(train);
    for (let index = 0; index < count; index++) {
        ticks += rmtPulseTicks(train, index);
    }
    return ticks;
}

// The changes of level of a PWM segment are numbered from its start: change
// 2k starts its period k (the pin goes active), change 2k + 1 ends the active
// share of that period (the pin goes idle). Walking them by number never
// loops on a rounding of their times.

/**
 * Whether a segment changes level at all (a duty cycle of 0 or 1 does not).
 */
export function pwmToggles(segment: PwmSegment): boolean {
    return segment.duty > 0 && segment.duty < 1;
}

/**
 * The time of change 'n' of a segment, µs.
 */
export function pwmChangeTime(segment: PwmSegment, n: number): number {
    const periodUs = 1000000 / segment.frequency;
    const period = Math.floor(n / 2);
    const activeShare = n % 2 === 1 ? segment.duty * periodUs : 0;
    return segment.from + period * periodUs + activeShare;
}

/**
 * The level of the pin after change 'n' of a segment: active after an even
 * change, idle after an odd one.
 */
export function pwmChangeLevel(segment: PwmSegment, n: number): number {
    const active = segment.invert ? 0 : 1;
    return n % 2 === 0 ? active : 1 - active;
}

/**
 * The number of the first change of a segment after 'time'.
 */
export function pwmFirstChangeAfter(segment: PwmSegment, time: number): number {
    if (time < segment.from) {
        return 0;
    }

    // the period 'time' falls in: its end of activity, else the next start
    const periodUs = 1000000 / segment.frequency;
    const period = Math.floor((time - segment.from) / periodUs);
    const activeEnd = 2 * period + 1;
    return pwmChangeTime(segment, activeEnd) > time ? activeEnd : activeEnd + 1;
}

/**
 * The level of the pin just before 'time' (within the segment, or at its
 * end): a change at 'time' itself does not count yet.
 */
export function pwmLevelBefore(segment: PwmSegment, time: number): number {
    const active = segment.invert ? 0 : 1;
    if (segment.duty >= 1) {
        return active;
    }
    if (segment.duty <= 0) {
        return 1 - active;
    }

    // the last change strictly before 'time' gives it
    let next = pwmFirstChangeAfter(segment, time);
    while (next > 0 && pwmChangeTime(segment, next - 1) >= time) {
        next--;
    }
    return next === 0 ? 1 - active : pwmChangeLevel(segment, next - 1);
}

/**
 * The level of the pin at 'time' (within the segment).
 */
export function pwmLevelAt(segment: PwmSegment, time: number): number {
    const active = segment.invert ? 0 : 1;
    if (segment.duty >= 1) {
        return active;
    }
    if (segment.duty <= 0) {
        return 1 - active;
    }

    // the change before 'time' gives it
    const next = pwmFirstChangeAfter(segment, time);
    return next === 0 ? 1 - active : pwmChangeLevel(segment, next - 1);
}

// A recording: what every pin did from 'start' to 'end'.
export interface Recording {
    traces: PinTrace[];                  // per pin
    start:  number;                      // µs
    end:    number;
}

/**
 * The index of the first state after 'time' (states.length when none), by
 * bisection.
 */
export function firstStateAfter(states: StateList, time: number): number {
    let low = 0;
    let high = states.length;
    while (low < high) {
        const middle = (low + high) >> 1;
        if (states.time(middle) <= time) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

/**
 * A copy of what 'traces' hold from 'start' to 'end': for each pin, its
 * state at the start (the last one before it) and its changes up to the
 * end, the busy times that touch that window, its PWM segments there
 * (those still running end at 'end') and its RMT sends there.
 */
export function copyTraces(traces: PinTrace[], start: number, end: number): PinTrace[] {
    return traces.map(trace => {
        const from = Math.max(0, firstStateAfter(trace.states, start) - 1);
        const to = firstStateAfter(trace.states, end);
        const busy = trace.busy.filter(interval => interval.to >= start && interval.from <= end);
        const touching = trace.pwm.filter(segment => segment.to > start && segment.from < end);
        const pwm = touching.map(segment => ({ ...segment, to: Math.min(segment.to, end) }));
        const rmt = trace.rmt.filter(train => train.to > start && train.from < end);
        return { states: trace.states.slice(from, to), busy, pwm, rmt };
    });
}

// ### Edges, for the trigger of the analyzer ##################################

// The edges that trigger: from 0 to 1, from 1 to 0, or either.
export type TriggerEdge = "rising" | "falling" | "both";

/**
 * Whether going from 'previous' to 'next' is an edge of the kind 'edge'.
 */
function isEdge(edge: TriggerEdge, previous: number, next: number): boolean {
    if (edge === "rising") {
        return previous === 0 && next !== 0;
    }
    if (edge === "falling") {
        return previous !== 0 && next === 0;
    }
    return (previous !== 0) !== (next !== 0);
}

/**
 * The time of the first change of state after 'time' that is an edge of
 * the kind 'edge'; null when none.
 */
function stateEdgeAfter(states: StateList, edge: TriggerEdge, time: number): number | null {
    for (let index = Math.max(1, firstStateAfter(states, time)); index < states.length; index++) {
        if (isEdge(edge, states.level(index - 1), states.level(index))) {
            return states.time(index);
        }
    }
    return null;
}

/**
 * The time of the first PWM edge of the kind 'edge' after 'time' (the
 * segments are in time order: the first one found is the earliest); null
 * when none. Its levels alternate: the right kind is one of the next two
 * changes.
 */
function pwmEdgeAfter(segments: PwmSegment[], edge: TriggerEdge, time: number): number | null {
    for (const segment of segments) {
        if (segment.to <= time || !pwmToggles(segment)) {
            continue;
        }

        // its next two changes after 'time' (its start included)
        let change = time < segment.from ? 0 : pwmFirstChangeAfter(segment, time);
        for (let tries = 0; tries < 2; tries++, change++) {
            const at = pwmChangeTime(segment, change);
            if (at >= segment.to) {
                break;
            }
            const level = pwmChangeLevel(segment, change);
            if (isEdge(edge, 1 - level, level)) {
                return at;
            }
        }
    }
    return null;
}

/**
 * The time of the first RMT edge of the kind 'edge' after 'time' (a send
 * starts from the pin low and ends low); null when none.
 */
function rmtEdgeAfter(trains: RmtTrain[], edge: TriggerEdge, time: number): number | null {
    for (const train of trains) {
        if (train.to <= time) {
            continue;
        }

        // each pulse that changes the level, then the end of the send
        let at = train.from;
        let previous = 0;
        const count = rmtPulseCount(train);
        for (let index = 0; index < count; index++) {
            const level = rmtPulseLevel(train, index);
            if (at > time && isEdge(edge, previous, level)) {
                return at;
            }
            previous = level;
            at += rmtPulseTicks(train, index) * train.tickUs;
        }
        if (at > time && isEdge(edge, previous, 0)) {
            return at;
        }
    }
    return null;
}

/**
 * The time of the first edge of the kind 'edge' a pin made after 'time':
 * a change of its state, of its PWM signal or of an RMT send; null when
 * none.
 */
export function firstEdgeAfter(trace: PinTrace, edge: TriggerEdge, time: number): number | null {
    const found = [
        stateEdgeAfter(trace.states, edge, time),
        pwmEdgeAfter(trace.pwm, edge, time),
        rmtEdgeAfter(trace.rmt, edge, time),
    ].filter(at => at !== null);
    return found.length > 0 ? Math.min(...found) : null;
}
