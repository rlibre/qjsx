// Builds a Markdown table comparing microbench.js results of qjs-original and qjs-split.
// Usage: node bench-table.js original=<file> split=<file> [original=<file> split=<file> ...]
// Several files per version are averaged.
'use strict';
const fs = require('fs');

// { testName: time in ns, total: score } from one microbench.js output
function parseBench(file) {
    const times = {};
    for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
        const row = line.trim().match(/^(\S+)\s+(\d+)\s+([\d.]+)/);
        if (row) {
            times[row[1]] = Number(row[3]);
        }
        const total = line.match(/total\s+([\d.]+)/);
        if (total) {
            times.total = Number(total[1]);
        }
    }
    return times;
}

function average(results) {
    const sums = {};
    for (const times of results) {
        for (const [name, value] of Object.entries(times)) {
            sums[name] = (sums[name] || 0) + value;
        }
    }
    const averages = {};
    for (const [name, sum] of Object.entries(sums)) {
        averages[name] = sum / results.length;
    }
    return averages;
}

const filesByVersion = { original: [], split: [] };
for (const argument of process.argv.slice(2)) {
    const [version, file] = argument.split('=');
    filesByVersion[version].push(file);
}
const original = average(filesByVersion.original.map(parseBench));
const split = average(filesByVersion.split.map(parseBench));

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
