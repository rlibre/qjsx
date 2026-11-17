/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Builds a Markdown table comparing microbench.js results of qjs-original and qjs-split.
// Usage: node bench-table.js original=<file> split=<file> [original=<file> split=<file> ...]
// Several files per version are averaged.
'use strict';
const fs = require('fs');

/**
 * { testName: time in ns, total: score } from one microbench.js output
 */
function parseBench(file) {
    const times = {};

    for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
        // a test line: name, iterations, time in ns
        const row = line.trim().match(/^(\S+)\s+(\d+)\s+([\d.]+)/);
        if (row) {
            times[row[1]] = Number(row[3]);
        }

        // the total line: the score
        const total = line.match(/total\s+([\d.]+)/);
        if (total) {
            times.total = Number(total[1]);
        }
    }

    return times;
}

/**
 * The average of each value over the results of several runs.
 */
function average(results) {
    // sum each value over the runs
    const sums = {};

    for (const times of results) {
        for (const [name, value] of Object.entries(times)) {
            sums[name] = (sums[name] || 0) + value;
        }
    }

    // then divide by the number of runs
    const averages = {};

    for (const [name, sum] of Object.entries(sums)) {
        averages[name] = sum / results.length;
    }

    return averages;
}

// the result files of each version, from the arguments
const filesByVersion = { original: [], split: [] };

for (const argument of process.argv.slice(2)) {
    const [version, file] = argument.split('=');
    filesByVersion[version].push(file);
}

// the average of each version
const original = average(filesByVersion.original.map(parseBench));
const split = average(filesByVersion.split.map(parseBench));

// one row per test, the total last: both times and the difference in %
const testNames = Object.keys(original).filter(name => name !== 'total').concat(['total']);
const lines = [
    '| Test | Original (ns) | Découpé (ns) | Écart |',
    '|---|---:|---:|---:|',
];

for (const name of testNames) {
    const difference = (split[name] - original[name]) / original[name] * 100;
    const sign = difference > 0 ? '+' : '';
    const label = name === 'total' ? '**total**' : name;
    lines.push(`| ${label} | ${original[name].toFixed(2)} | ${split[name].toFixed(2)} | ${sign}${difference.toFixed(1)} % |`);
}

console.log(lines.join('\n'));
