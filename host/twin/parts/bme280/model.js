/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A Bosch BME280 (temperature, humidity, pressure) on an I2C bus, as its
// datasheet describes it (BST-BME280-DS002), enough for a driver written for
// the real chip:
//   - registers: chip id 0xD0 = 0x60, reset 0xE0 (0xB6), ctrl_hum 0xF2,
//     status 0xF3, ctrl_meas 0xF4 (oversampling, mode), config 0xF5, the
//     data 0xF7..0xFE (pressure, temperature: 20 bits; humidity: 16 bits),
//     the calibration 0x88..0xA1 and 0xE1..0xE7 (fixed values of a real
//     chip);
//   - writes come as register / value pairs, reads move the pointer;
//   - a measurement is instant: "forced" measures once then goes back to
//     sleep, "normal" measures at each read of the data; a quantity whose
//     oversampling is 0 reads 0x80000 (0x8000 for humidity), as the chip;
//   - the raw values are those that the compensation formulas of the
//     datasheet (section 8.1) turn into the temperature, humidity and
//     pressure set on the page (found by bisection, as the formulas are
//     monotonic).
//
//   { "id": "meteo", "model": "bme280", "label": "Weather", "i2c": { "sda": 8, "scl": 9 },
//     "address": "0x76", "initial": { "temperature": 21.5, "humidity": 40, "pressure": 1013.25 } }
import { Part, Registers, parseAddress } from "qjsx:twin";

// Registers.
const REG_CALIBRATION_1 = 0x88;       // dig_T1..dig_P9 (24 bytes), then 0xA1 = dig_H1
const REG_ID = 0xd0;
const REG_RESET = 0xe0;
const REG_CALIBRATION_2 = 0xe1;       // dig_H2..dig_H6 (7 bytes)
const REG_CTRL_HUM = 0xf2;
const REG_CTRL_MEAS = 0xf4;
const REG_DATA = 0xf7;                // press (3), temp (3), hum (2)

const CHIP_ID = 0x60;
const RESET_WORD = 0xb6;
const SKIPPED_20 = 0x80000;           // a 20-bit quantity not measured
const SKIPPED_16 = 0x8000;            // humidity not measured

// The calibration of a real chip (the trimming values Bosch writes in each one).
const CALIBRATION = {
    T1: 28485, T2: 26735, T3: 50,
    P1: 36908, P2: -10744, P3: 3024, P4: 7366, P5: -115, P6: -7, P7: 9900, P8: -10230, P9: 4285,
    H1: 75, H2: 359, H3: 0, H4: 343, H5: 50, H6: 30,
};

// The ranges of the chip (datasheet, table 1).
const RANGES = {
    temperature: [-40, 85],           // °C
    humidity: [0, 100],               // %RH
    pressure: [300, 1100],            // hPa
};

// ### Compensation (datasheet 8.1, floating point) ############################

/**
 * Temperature in °C and t_fine from the raw temperature.
 */
function compensateTemperature(adcT) {
    const { T1, T2, T3 } = CALIBRATION;
    const var1 = (adcT / 16384 - T1 / 1024) * T2;
    const var2 = (adcT / 131072 - T1 / 8192) ** 2 * T3;
    const tFine = var1 + var2;
    return { temperature: tFine / 5120, tFine };
}

/**
 * Pressure in Pa from the raw pressure and t_fine.
 */
function compensatePressure(adcP, tFine) {
    const { P1, P2, P3, P4, P5, P6, P7, P8, P9 } = CALIBRATION;
    let var1 = tFine / 2 - 64000;
    let var2 = (var1 * var1 * P6) / 32768;
    var2 = var2 + var1 * P5 * 2;
    var2 = var2 / 4 + P4 * 65536;
    var1 = ((P3 * var1 * var1) / 524288 + P2 * var1) / 524288;
    var1 = (1 + var1 / 32768) * P1;
    if (var1 === 0) {
        return 0;
    }

    let pressure = 1048576 - adcP;
    pressure = ((pressure - var2 / 4096) * 6250) / var1;
    var1 = (P9 * pressure * pressure) / 2147483648;
    var2 = (pressure * P8) / 32768;
    return pressure + (var1 + var2 + P7) / 16;
}

/**
 * Relative humidity in % from the raw humidity and t_fine (not clamped: the
 * bisection needs a monotonic function).
 */
function compensateHumidity(adcH, tFine) {
    const { H1, H2, H3, H4, H5, H6 } = CALIBRATION;
    let humidity = tFine - 76800;
    humidity = (adcH - (H4 * 64 + (H5 / 16384) * humidity))
        * ((H2 / 65536) * (1 + (H6 / 67108864) * humidity * (1 + (H3 / 67108864) * humidity)));
    return humidity * (1 - (H1 * humidity) / 524288);
}

/**
 * The integer 'raw' in [0, max] for which 'compensate(raw)' is closest to
 * 'target' (the formulas are monotonic: a bisection).
 */
function invert(compensate, target, max) {
    const increasing = compensate(max) > compensate(0);
    let low = 0;
    let high = max;
    while (high - low > 1) {
        const middle = Math.floor((low + high) / 2);
        const below = compensate(middle) < target;
        if (below === increasing) {
            low = middle;
        } else {
            high = middle;
        }
    }

    const lowError = Math.abs(compensate(low) - target);
    const highError = Math.abs(compensate(high) - target);
    return lowError <= highError ? low : high;
}

// ### The model ###############################################################

/**
 * A quantity of the config ("initial"), checked against the range of the
 * chip; 'fallback' when absent.
 */
function initialValue(initial, name, fallback) {
    const value = initial?.[name] ?? fallback;
    const [min, max] = RANGES[name];
    if (typeof value !== "number" || value < min || value > max) {
        throw new Error(`"initial.${name}" must be a number from ${min} to ${max}`);
    }
    return value;
}

/**
 * The bytes of the calibration registers, little endian as the chip.
 */
function calibrationBytes() {
    const { T1, T2, T3, P1, P2, P3, P4, P5, P6, P7, P8, P9, H1, H2, H3, H4, H5, H6 } = CALIBRATION;
    const first = new DataView(new ArrayBuffer(26));
    [T1, T2, T3, P1, P2, P3, P4, P5, P6, P7, P8, P9].forEach((value, index) => {
        first.setUint16(index * 2, value & 0xffff, true);
    });
    first.setUint8(25, H1);   // 0xA1 (0xA0 is not used)

    // dig_H4 and dig_H5 are 12-bit values sharing 0xE5
    const second = new Uint8Array(7);
    second[0] = H2 & 0xff;
    second[1] = (H2 >> 8) & 0xff;
    second[2] = H3;
    second[3] = (H4 >> 4) & 0xff;
    second[4] = (H4 & 0x0f) | ((H5 & 0x0f) << 4);
    second[5] = (H5 >> 4) & 0xff;
    second[6] = H6 & 0xff;
    return { first: new Uint8Array(first.buffer), second };
}

export default class Bme280 extends Part {
    static controls = [
        { name: "temperature", type: "knob", min: RANGES.temperature[0], max: RANGES.temperature[1], step: 0.1, unit: "°C" },
        { name: "humidity", type: "knob", min: RANGES.humidity[0], max: RANGES.humidity[1], step: 0.5, unit: "%" },
        { name: "pressure", type: "knob", min: RANGES.pressure[0], max: RANGES.pressure[1], step: 0.5, unit: "hPa" },
        { name: "mode", type: "value" },
    ];

    /**
     * The chip after power-on, at its address on its bus.
     */
    constructor(config, board) {
        super(config, board);
        const address = parseAddress(config.address ?? "0x76");
        if (address !== 0x76 && address !== 0x77) {
            throw new Error('"address" of a BME280 is "0x76" (SDO to ground) or "0x77" (SDO to the supply)');
        }

        // what the page sets
        this._temperature = initialValue(config.initial, "temperature", 21.5);
        this._humidity = initialValue(config.initial, "humidity", 40);
        this._pressure = initialValue(config.initial, "pressure", 1013.25);

        // the registers, and what the program does to them
        this._registers = new Registers(256, { writes: "pairs" });
        this._powerOn();
        this._registers.onWrite(REG_RESET, value => this._onReset(value));
        this._registers.onWrite(REG_CTRL_MEAS, () => this._onCtrlMeas());
        this._registers.onRead((start, length) => this._onRead(start, length));
        board.i2c(config.i2c).attach(address, this._registers);
        this._publishState();
    }

    /**
     * "set" with { temperature, humidity, pressure } (any of them): the new
     * values, measured at the next measurement (at once in normal mode).
     */
    onAction(action, args) {
        if (action !== "set" || typeof args !== "object" || args === null) {
            return;
        }

        for (const name of ["temperature", "humidity", "pressure"]) {
            const value = args[name];
            if (typeof value === "number") {
                const [min, max] = RANGES[name];
                this[`_${name}`] = Math.min(max, Math.max(min, value));
            }
        }
        this._publishState();
    }

    /**
     * The registers after power-on (and after a soft reset): id,
     * calibration, everything else 0, the data as "not measured".
     */
    _powerOn() {
        const registers = this._registers;
        registers.bytes.fill(0);
        registers.set(REG_ID, CHIP_ID);
        const calibration = calibrationBytes();
        registers.set(REG_CALIBRATION_1, calibration.first);
        registers.set(REG_CALIBRATION_2, calibration.second);
        this._setData(SKIPPED_20, SKIPPED_20, SKIPPED_16);
    }

    /**
     * 0xB6 written to the reset register: the chip starts again.
     */
    _onReset(value) {
        if (value === RESET_WORD) {
            this._powerOn();
            this._publishState();
        }
    }

    /**
     * ctrl_meas written: a forced measurement is made at once, then the chip
     * goes back to sleep; normal mode measures too.
     */
    _onCtrlMeas() {
        const mode = this._mode();
        if (mode === "forced") {
            this._measure();
            const ctrlMeas = this._registers.get(REG_CTRL_MEAS);
            this._registers.set(REG_CTRL_MEAS, ctrlMeas & 0xfc);
        } else if (mode === "normal") {
            this._measure();
        }
        this._publishState();
    }

    /**
     * A read: in normal mode, the data are measured again when the read
     * reaches them (0xF7 to 0xFE).
     */
    _onRead(start, length) {
        const readsData = start <= 0xfe && start + length > REG_DATA;
        if (this._mode() === "normal" && readsData) {
            this._measure();
        }
    }

    /**
     * The mode of ctrl_meas: "sleep", "forced" or "normal".
     */
    _mode() {
        const bits = this._registers.get(REG_CTRL_MEAS) & 0x03;
        if (bits === 0) {
            return "sleep";
        }
        return bits === 3 ? "normal" : "forced";
    }

    /**
     * A measurement: the raw values of the quantities whose oversampling is
     * not 0 (ctrl_meas for temperature and pressure, ctrl_hum for humidity).
     */
    _measure() {
        const ctrlMeas = this._registers.get(REG_CTRL_MEAS);
        const measureTemperature = (ctrlMeas >> 5) & 0x07;
        const measurePressure = (ctrlMeas >> 2) & 0x07;
        const measureHumidity = this._registers.get(REG_CTRL_HUM) & 0x07;

        // temperature first: t_fine is what the other two depend on
        const adcT = invert(raw => compensateTemperature(raw).temperature, this._temperature, 0xfffff);
        const { tFine } = compensateTemperature(adcT);
        const adcP = invert(raw => compensatePressure(raw, tFine), this._pressure * 100, 0xfffff);
        const adcH = invert(raw => compensateHumidity(raw, tFine), this._humidity, 0xffff);

        this._setData(measurePressure ? adcP : SKIPPED_20, measureTemperature ? adcT : SKIPPED_20,
            measureHumidity ? adcH : SKIPPED_16);
    }

    /**
     * The data registers: pressure and temperature on 20 bits (msb, lsb,
     * xlsb << 4), humidity on 16 bits (msb, lsb).
     */
    _setData(adcP, adcT, adcH) {
        this._registers.set(REG_DATA, [
            (adcP >> 12) & 0xff, (adcP >> 4) & 0xff, (adcP & 0x0f) << 4,
            (adcT >> 12) & 0xff, (adcT >> 4) & 0xff, (adcT & 0x0f) << 4,
            (adcH >> 8) & 0xff, adcH & 0xff,
        ]);
    }

    /**
     * The state for the page.
     */
    _publishState() {
        this.publish({
            temperature: this._temperature,
            humidity: this._humidity,
            pressure: this._pressure,
            mode: this._mode(),
        });
    }
}
