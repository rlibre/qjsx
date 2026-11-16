/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Random numbers: the global crypto (getRandomValues, randomUUID, web
// standard) and randomBytes of sys/crypto; then, given two twins, the program
// tests/fixtures/twinrandom run twice on each: the same numbers at each run
// on the twin with "randomSeed", other numbers on the twin without one.
//
//   qjsx run -A tests/random.test.js [<target of the seeded twin> <target of a twin without seed>]
import * as fs from "sys/fs";
import { randomBytes } from "sys/crypto";
import { build } from "qjsx:build";
import { Device } from "qjsx:device";
import { upload } from "qjsx:deploy";
import { KIND } from "qjsx:protocol";
import { check, equal, throws, report } from "./assert.js";

const SEEDED_TARGET = process.argv[2];
const FREE_TARGET = process.argv[3];
const PROJECT = "tests/fixtures/twinrandom";
const WAIT_TIMEOUT = 8000;
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;

// ### crypto.getRandomValues ###################################################

// integer arrays: filled in place, the same array returned
const bytes = new Uint8Array(64);
check(crypto.getRandomValues(bytes) === bytes, "getRandomValues returns the array it was given");
check(bytes.some(byte => byte !== 0), "the bytes are filled");
check(crypto.getRandomValues(new Uint32Array(4)) instanceof Uint32Array, "a Uint32Array is accepted");
check(crypto.getRandomValues(new BigUint64Array(2)) instanceof BigUint64Array, "a BigUint64Array is accepted");
check(crypto.getRandomValues(new Int8Array(0)).length === 0, "an empty array is accepted");

// a view fills only its own bytes
const whole = new Uint8Array(16);
crypto.getRandomValues(whole.subarray(4, 12));
check(whole.slice(0, 4).every(byte => byte === 0) && whole.slice(12).every(byte => byte === 0), "a subarray fills only its bytes");

// anything else is refused
throws(() => crypto.getRandomValues(new Float32Array(2)), /^TypeError: .*not a float/, "a Float32Array is refused");
throws(() => crypto.getRandomValues(new Float64Array(2)), /^TypeError: .*not a float/, "a Float64Array is refused");
throws(() => crypto.getRandomValues(new Float16Array(2)), /^TypeError: .*not a float/, "a Float16Array is refused");
throws(() => crypto.getRandomValues(new DataView(new ArrayBuffer(4))), /^TypeError: /, "a DataView is refused");
throws(() => crypto.getRandomValues(new ArrayBuffer(4)), /^TypeError: /, "an ArrayBuffer is refused");
throws(() => crypto.getRandomValues([1, 2]), /^TypeError: /, "an array is refused");

// 65536 bytes at most, as the standard
equal(crypto.getRandomValues(new Uint8Array(65536)).length, 65536, "65536 bytes are accepted");
throws(() => crypto.getRandomValues(new Uint8Array(65537)), /^QuotaExceededError: /, "65537 bytes: QuotaExceededError");
throws(() => crypto.getRandomValues(new Uint32Array(16385)), /^QuotaExceededError: /, "the limit counts bytes, not elements");

// ### crypto.randomUUID, randomBytes ###########################################

const uuid = crypto.randomUUID();
check(UUID.test(uuid), `randomUUID is a UUID of version 4 (${uuid})`);
check(crypto.randomUUID() !== uuid, "two UUIDs differ");

const random = randomBytes(32);
check(random instanceof Uint8Array && random.length === 32, "randomBytes gives a Uint8Array of the size asked");
equal(randomBytes(0).length, 0, "randomBytes(0) is empty");
check(randomBytes(32).toHex() !== random.toHex(), "two draws differ");
throws(() => randomBytes(-1), /^RangeError: /, "a negative size is refused");

// ### Reproducible random numbers of a twin ####################################

// Runs the program twice on the twin at 'target': its two lines.
async function twoRuns(target, projectBuild) {
    const lines = [];
    const onLog = (stream, text) => {
        for (const line of text.split("\n").filter(item => item.startsWith("random: "))) {
            lines.push(line);
        }
    };

    // the program, installed, then run twice
    const device = await Device.open(target, { onLog });
    await device.hello();
    await device.request(KIND.STOP);
    await upload(device, fs.readFileSync(projectBuild.file));
    for (let run = 1; run <= 2; run++) {
        await device.request(KIND.RUN);
        const end = Date.now() + WAIT_TIMEOUT;
        while (lines.length < run && Date.now() < end) {
            await new Promise(resolve => setTimeout(resolve, 20));
        }
    }

    device.close();
    return lines;
}

if (SEEDED_TARGET !== undefined && FREE_TARGET !== undefined) {
    const projectBuild = build(PROJECT);

    // with a seed: the same values at each run
    const seeded = await twoRuns(SEEDED_TARGET, projectBuild);
    equal(seeded.length, 2, "the program ran twice on the twin with a seed");
    check(seeded.length === 2 && seeded[0] === seeded[1], `the same random numbers at each run with a seed (${seeded.join(" / ")})`);

    // without: other values at each run
    const free = await twoRuns(FREE_TARGET, projectBuild);
    equal(free.length, 2, "the program ran twice on the twin without a seed");
    check(free.length === 2 && free[0] !== free[1], "other random numbers at each run without a seed");
    check(free.length === 2 && seeded.length === 2 && free[0] !== seeded[0], "the seeded sequence is not the free one");

    fs.rmSync(`${PROJECT}/build`, { recursive: true, force: true });
}

report("random");
