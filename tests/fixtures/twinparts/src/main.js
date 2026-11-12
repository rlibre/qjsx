/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A program for the parts wired by the project in the twin
// (tests/twinparts.test.js): a BME280 at 0x76 read as on a real board (its
// calibration, a forced measurement, the compensation of its datasheet),
// the echo part of the project at 0x50, a second I2C bus with nothing on it,
// a potentiometer read by device/adc, and a comet on the strip of 5 WS2812
// LEDs. It prints a line for each result, then the weather and the voltage
// of the potentiometer each time they change.
import { Adc } from "device/adc";
import { I2c } from "device/i2c";
import { RgbLed } from "stdlib/rgbled";

// --- the potentiometer on GPIO6, through device/adc
const gain = new Adc(6, { range: 3.3 });
console.log(`adc bits ${gain.bits}`);
try {
    new Adc(21);
} catch (error) {
    console.log(`adc on GPIO21 ${error.code}`);
}

let lastGain = "";
setInterval(() => {
    const line = `gain ${gain.readVoltage().toFixed(2)} V`;
    if (line !== lastGain) {
        lastGain = line;
        console.log(line);
    }
}, 100);

const bus = new I2c(0, { sda: 8, scl: 9 });

// --- who answers
console.log(`probe 0x76 ${bus.probeSync(0x76)}`);
console.log(`probe 0x51 ${bus.probeSync(0x51)}`);

// --- a bus with nothing on it: every transfer ends with a NACK
const empty = new I2c(1, { sda: 4, scl: 5 });
console.log(`empty bus: probe 0x3c ${empty.probeSync(0x3c)}`);
try {
    empty.device(0x3c).writeSync([0x00, 0xaf]);
} catch (error) {
    console.log(`empty bus: write ${error.code}`);
}

// --- a comet going to and fro along the strip: a bright head, a trail
// that fades, a hue that slowly drifts
const COUNT = 5;
const strip = new RgbLed(21, { count: COUNT });
const brightness = new Array(COUNT).fill(0);
let position = 0;
let direction = 1;
let hue = 190;
setInterval(async () => {
    // the trail fades, the head lights its LED
    for (let index = 0; index < COUNT; index++) {
        brightness[index] *= 0.6;
    }
    brightness[Math.round(position)] = 1;

    // the colors, the head a little whiter than its trail
    for (let index = 0; index < COUNT; index++) {
        const saturation = brightness[index] > 0.95 ? 0.55 : 1;
        strip.setHsv(index, (hue + index * 12) % 360, saturation, brightness[index] * 0.9);
    }
    await strip.show();

    // the next step: bounce at the ends, the hue drifts
    position += direction * 0.5;
    if (position >= COUNT - 1 || position <= 0) {
        direction = -direction;
    }
    hue = (hue + 1.5) % 360;
}, 50);

// --- the echo part: gives back what was written, reversed
const echo = bus.device(0x50);
console.log(`echo ${Array.from(echo.writeReadSync([1, 2, 3], 3)).join(",")}`);

// --- the BME280: its id, its calibration
const bme = bus.device(0x76);
console.log(`chip id 0x${bme.writeReadSync([0xd0], 1)[0].toString(16)}`);

const first = new DataView(bme.writeReadSync([0x88], 26).buffer);
const second = bme.writeReadSync([0xe1], 7);
const calibration = {
    T1: first.getUint16(0, true), T2: first.getInt16(2, true), T3: first.getInt16(4, true),
    P1: first.getUint16(6, true), P2: first.getInt16(8, true), P3: first.getInt16(10, true),
    P4: first.getInt16(12, true), P5: first.getInt16(14, true), P6: first.getInt16(16, true),
    P7: first.getInt16(18, true), P8: first.getInt16(20, true), P9: first.getInt16(22, true),
    H1: first.getUint8(25),
    H2: (second[1] << 8 | second[0]) << 16 >> 16,
    H3: second[2],
    H4: (second[3] << 4 | (second[4] & 0x0f)) << 20 >> 20,
    H5: (second[5] << 4 | (second[4] >> 4)) << 20 >> 20,
    H6: second[6] << 24 >> 24,
};

// The compensation of the datasheet (8.1, floating point): °C, Pa, %.
function compensate(adcT, adcP, adcH) {
    const c = calibration;

    // temperature, and t_fine for the others
    const t1 = (adcT / 16384 - c.T1 / 1024) * c.T2;
    const t2 = (adcT / 131072 - c.T1 / 8192) ** 2 * c.T3;
    const tFine = t1 + t2;

    // pressure
    let var1 = tFine / 2 - 64000;
    let var2 = (var1 * var1 * c.P6) / 32768;
    var2 = var2 + var1 * c.P5 * 2;
    var2 = var2 / 4 + c.P4 * 65536;
    var1 = ((c.P3 * var1 * var1) / 524288 + c.P2 * var1) / 524288;
    var1 = (1 + var1 / 32768) * c.P1;
    let pressure = 1048576 - adcP;
    pressure = ((pressure - var2 / 4096) * 6250) / var1;
    pressure = pressure + ((c.P9 * pressure * pressure) / 2147483648 + (pressure * c.P8) / 32768 + c.P7) / 16;

    // humidity
    let humidity = tFine - 76800;
    humidity = (adcH - (c.H4 * 64 + (c.H5 / 16384) * humidity))
        * ((c.H2 / 65536) * (1 + (c.H6 / 67108864) * humidity * (1 + (c.H3 / 67108864) * humidity)));
    humidity = humidity * (1 - (c.H1 * humidity) / 524288);

    return { temperature: tFine / 5120, pressure, humidity: Math.min(100, Math.max(0, humidity)) };
}

// A forced measurement (oversampling 1 for all three), then its data.
function measure() {
    bme.writeSync([0xf2, 0x01, 0xf4, 0x25]);
    const data = bme.writeReadSync([0xf7], 8);
    const adcP = (data[0] << 12) | (data[1] << 4) | (data[2] >> 4);
    const adcT = (data[3] << 12) | (data[4] << 4) | (data[5] >> 4);
    const adcH = (data[6] << 8) | data[7];
    return compensate(adcT, adcP, adcH);
}

// --- the weather, each time it changes
let last = "";
setInterval(() => {
    const { temperature, humidity, pressure } = measure();
    const line = `weather ${temperature.toFixed(2)} C ${humidity.toFixed(1)} % ${(pressure / 100).toFixed(2)} hPa`;
    if (line !== last) {
        last = line;
        console.log(line);
    }
}, 100);
