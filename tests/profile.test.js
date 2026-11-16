/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The sampling profiler of the agent ("qjsx profile", QJSX_PROFILER, on in
// the desktop agent): the program tests/fixtures/profiled works about half
// of the time in spin(), called by the method step(); a profile of 1.5 s
// must say so, with the functions named from the debug files (a method
// named by its source), and the agent must answer PROFILE_STOP without a
// PROFILE_START.
//
//   qjsx run -A tests/profile.test.js tcp:127.0.0.1:<agent port>
import * as fs from "sys/fs";
import { join } from "stdlib/path";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KEY, KIND } from "qjsx:protocol";
import { PackageIndex } from "qjsx:debuginfo";
import { readReport, functionTimes, profileText } from "qjsx:profile";
import { check, equal, report } from "./assert.js";

const TARGET = process.argv[2];
const PROJECT = "tests/fixtures/profiled";

// --- the program, running on the desktop agent
const device = await Device.open(TARGET, {});
await device.hello();
const projectBuild = build(PROJECT);
await device.request(KIND.STOP);
await upload(device, fs.readFileSync(projectBuild.file));
await device.request(KIND.RUN);
await new Promise(resolve => setTimeout(resolve, 300));

// --- a stop without start: an empty profile
const empty = readReport(await device.request(KIND.PROFILE_STOP));
equal(empty.samples + empty.idle, 0, "PROFILE_STOP without PROFILE_START: no sample");

// --- a profile of 1.5 s
await device.request(KIND.PROFILE_START, { [KEY.INTERVAL]: 1 });
await new Promise(resolve => setTimeout(resolve, 1500));
const profile = readReport(await device.request(KIND.PROFILE_STOP));
const all = profile.samples + profile.idle;
check(all > 100, `samples taken (${all}; the interval of the PC's clock may be longer than 1 ms)`);
check(profile.durationMs >= 1400, `the duration (${profile.durationMs} ms)`);
const busy = profile.samples / all;
check(busy > 0.3 && busy < 0.8, `the JS thread works about half of the time (${busy.toFixed(2)})`);
check(profile.modules.includes("src/main.js"), `the module names (${profile.modules.join(", ")})`);

// --- the functions, named with the debug files of the package
const pkg = new PackageIndex([join(PROJECT, "build/packages")]).find(projectBuild.buildId);
check(pkg !== undefined, "the package of the profiled program is found");
const rows = functionTimes(profile, pkg);
equal(rows[0]?.name, "spin", "spin() takes the most time itself");
check(rows[0]?.self > profile.samples * 0.7, `... most of the busy samples (${rows[0]?.self} of ${profile.samples})`);
const step = rows.find(row => row.name === "step");
check(step !== undefined && step.total >= rows[0].self, "step(), a method named from its source, has spin() in its total");
check(rows[0]?.where === "src/main.js:10", `... and the place of spin() (${rows[0]?.where})`);
check(profileText(profile, pkg).includes("spin  src/main.js:10"), "the text of the profile lists it");

// --- a second profile works too (the table is made again)
await device.request(KIND.PROFILE_START, { [KEY.INTERVAL]: 5 });
await new Promise(resolve => setTimeout(resolve, 300));
const second = readReport(await device.request(KIND.PROFILE_STOP));
equal(second.intervalMs, 5, "the interval asked");
check(second.samples + second.idle > 10, "a second profile samples");

await device.request(KIND.STOP);
device.close();
fs.rmSync(join(PROJECT, "build"), { recursive: true, force: true });
report("profile");
