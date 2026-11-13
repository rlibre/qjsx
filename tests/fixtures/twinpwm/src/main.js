/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A program for device/pwm in the digital twin (tests/pwm.test.js), whose
// board has the PWM of the ESP32-S3 (8 outputs, 4 timers, an 80 MHz clock,
// 14 bits). It prints what each call gives, one line each, then keeps its
// outputs 1.5 s for the test to read the pins, closes one, and ends (the
// others stop with the program).
import { Pwm } from "device/pwm";

// What a call throws: the code of a system error, else the kind of error.
function outcome(run) {
    try {
        run();
        return "ok";
    } catch (error) {
        return error.code ?? error.name;
    }
}

// --- an output, its defaults: 1 kHz, 14 bits, off
const led = new Pwm(5);
console.log(`default: ${led.frequency} ${led.bits} ${led.duty}`);

// --- the duty cycle, rounded to a step of 1/16384
led.setDuty(0.25);
console.log(`quarter: ${led.duty}`);
led.setDuty(1 / 3);
console.log(`third: ${led.duty * 16384}`);
console.log(`duty 1.5: ${outcome(() => led.setDuty(1.5))}`);
console.log(`duty NaN: ${outcome(() => led.setDuty(NaN))}`);

// --- another frequency: the resolution follows, the duty cycle is kept;
// 3 kHz is rounded by the divider of the timer
led.setFrequency(20000);
console.log(`20 kHz: ${led.frequency} ${led.bits} ${led.duty * 2048}`);
led.setFrequency(3000);
console.log(`3 kHz: ${led.frequency.toFixed(1)} ${led.bits}`);

// --- refused
console.log(`too low: ${outcome(() => new Pwm(6, { frequency: 2 }))}`);
console.log(`too high: ${outcome(() => new Pwm(6, { frequency: 40000001 }))}`);
console.log(`no such pin: ${outcome(() => new Pwm(60))}`);
console.log(`pin taken: ${outcome(() => new Pwm(5))}`);

// --- four timers: four frequencies at once, a fifth refused; the same
// frequency shares a timer, which cannot leave it while all are taken
const full = new Pwm(6, { frequency: 100 });
const second = new Pwm(7, { frequency: 200 });
const inverted = new Pwm(8, { frequency: 300, duty: 0.5, invert: true });
console.log(`fifth frequency: ${outcome(() => new Pwm(9, { frequency: 400 }))}`);
const shared = new Pwm(9, { frequency: 100 });
console.log(`shared timer: ${shared.frequency}`);
console.log(`move: ${outcome(() => shared.setFrequency(500))} ${shared.frequency}`);

// --- eight outputs, a ninth refused
const others = [10, 11, 12].map(pin => new Pwm(pin, { frequency: 100 }));
console.log(`ninth output: ${outcome(() => new Pwm(13, { frequency: 100 }))}`);

// --- an output closed frees its timer: a servo at 50 Hz, 1.5 ms pulses
second.close();
console.log(`closed: ${outcome(() => second.setDuty(0.5))}`);
const servo = new Pwm(7, { frequency: 50 });
servo.setPulseWidth(1500);
console.log(`servo: ${servo.frequency} ${servo.duty * 16384}`);
console.log(`pulse too long: ${outcome(() => servo.setPulseWidth(20001))}`);

// --- always active: the pin at 1
full.setDuty(1);

// --- the pins read by the test, then one output closed, then the end
console.log(`outputs: ${[led, full, inverted, shared, servo, ...others].length}`);
console.log("pwm ready");
setTimeout(() => {
    led.close();
    console.log("pwm closed");
}, 1500);
