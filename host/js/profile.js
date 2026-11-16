/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The profile of a device ("qjsx profile"): the agent samples its JS thread
// (PROFILE_START / PROFILE_STOP, common/agent/profiler.*) and sends the
// distinct stacks it saw with their counts; this module turns them into the
// time of each function, named with the debug files of the package the
// device runs (qjsx:debuginfo).
//
//   self   the samples where the function was running itself (innermost)
//   total  the samples where it was on the stack (itself and what it calls),
//          counted once per sample even when it recurses
// The device keeps 8 frames per sample: a total of a function deeper than
// that is undercounted.
import { KEY } from "qjsx:protocol";

// The module index of a frame whose module the device had no room for.
const NO_MODULE = 0xFFFF;

/**
 * The answer of PROFILE_STOP as a plain object: { durationMs, intervalMs,
 * samples, idle, overflow, modules, stacks: [{ count, frames }] }.
 */
export function readReport(answer) {
    const stacks = (answer.get(KEY.STACKS) ?? []).map(([count, ...frames]) => ({ count, frames }));
    return {
        durationMs: answer.get(KEY.DURATION) ?? 0,
        intervalMs: answer.get(KEY.INTERVAL) ?? 0,
        samples: answer.get(KEY.SAMPLES) ?? 0,
        idle: answer.get(KEY.IDLE) ?? 0,
        overflow: answer.get(KEY.OVERFLOW) ?? 0,
        modules: answer.get(KEY.MODULES) ?? [],
        stacks,
    };
}

/**
 * Name and place of a frame: { key, name, where }. 'pkg': the DebugPackage
 * the device runs, or undefined (then numbers only).
 */
function describeFrame(frame, report, pkg) {
    const moduleIndex = frame >>> 16;
    const number = frame & 0xFFFF;
    const module = moduleIndex === NO_MODULE ? "(unknown module)" : report.modules[moduleIndex];
    const key = `${module}#${number}`;

    // without debug files: the numbers
    const script = pkg?.script(module);
    if (script === undefined) {
        return { key, name: `function #${number}`, where: module };
    }

    // the name, and the line where the function starts (1-based)
    const info = script.functions[number];
    const line = info === undefined ? undefined : script.lines.position(info.start).line + 1;
    const where = line === undefined ? module : `${module}:${line}`;
    return { key, name: script.functionName(number), where };
}

/**
 * The functions of a report, the most costly first: [{ name, where, self,
 * total }], self and total in samples.
 */
export function functionTimes(report, pkg) {
    const rows = new Map();   // key -> row

    // the row of a frame, made on first sight
    const rowOf = frame => {
        const { key, name, where } = describeFrame(frame, report, pkg);
        if (!rows.has(key)) {
            rows.set(key, { name, where, self: 0, total: 0 });
        }
        return { key, row: rows.get(key) };
    };

    for (const stack of report.stacks) {
        // self: the innermost frame
        if (stack.frames.length > 0) {
            rowOf(stack.frames[0]).row.self += stack.count;
        }

        // total: each function once per stack
        const counted = new Set();
        for (const frame of stack.frames) {
            const { key, row } = rowOf(frame);
            if (!counted.has(key)) {
                counted.add(key);
                row.total += stack.count;
            }
        }
    }

    return [...rows.values()].sort((a, b) => b.self - a.self || b.total - a.total);
}

/**
 * The text of a profile: a summary line, then the 'top' most costly
 * functions, shares of all the samples (working and idle).
 */
export function profileText(report, pkg, top = 20) {
    const all = report.samples + report.idle;
    const share = count => (all === 0 ? "0.0 %" : `${((count * 100) / all).toFixed(1)} %`);
    const lines = [];

    // the summary
    const seconds = (report.durationMs / 1000).toFixed(1);
    lines.push(`JS thread ${share(report.samples)} busy over ${seconds} s (${all} samples every ${report.intervalMs} ms, idle ${share(report.idle)})`);
    if (report.overflow > 0) {
        lines.push(`(${report.overflow} samples whose stack did not fit in the table of the device: not counted below)`);
    }
    if (pkg === undefined) {
        lines.push("(no debug files of the package the device runs: functions by number; build it in this project)");
    }

    // the functions
    lines.push("   self    total  function");
    for (const row of functionTimes(report, pkg).slice(0, top)) {
        lines.push(`${share(row.self).padStart(7)}  ${share(row.total).padStart(7)}  ${row.name}  ${row.where}`);
    }
    return lines.join("\n");
}
