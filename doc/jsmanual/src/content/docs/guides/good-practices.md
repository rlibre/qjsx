---
title: Good practices
description: Writing a program that fits a small board, with little polling, little memory and errors that do not stop it.
---

A board runs JavaScript about a hundred times slower than a PC, with a few hundred
kilobytes of JS heap when it has no PSRAM. A program that is fine on the PC, or on the
[digital twin](../twin/) (which runs at the speed of the PC), may keep the JS thread of
the board busy all the time. This page gathers what keeps a program light; the figures
come from an ESP32-S3 at 240 MHz without PSRAM.

## The JS thread of a board is slow

| On an ESP32-S3 without PSRAM | Time |
|---|---|
| `await null` | about 200 to 300 µs |
| `await` of a `setTimeout(0)` | about 0.6 to 1 ms |
| a loop that wakes up every 10 ms and does nothing | 20 to 35 % of the JS thread |

Every wake-up of a task (a ticker step, a timer, a resolved promise) costs time before
the program does anything. Three loops at 100 Hz, each doing a little work, are enough
to keep the JS thread of such a board busy about 90 % of the time: the callbacks then
wait, and the [debugger](../debugging/) answers slowly. The speed also depends on the
firmware of the board (the flash in QIO mode instead of DIO made it about 1.6 times
faster), never enough to make polling free.

## Wait for events, do not poll

A pin, a serial port or a USB function tells the program when something happens: the
program has nothing to run meanwhile.

```js
import { Gpio } from "device/gpio";

const button = new Gpio(0, { mode: "input", pull: "up" });
const led = new Gpio(5, { mode: "output" });

// not: a loop that reads the button every 10 ms
button.onChange(level => led.write(level === 0), { edge: "both", debounce: 20 });
```

- `gpio.onChange(fn)` for a level that changes, `await gpio.waitFor(level, { timeout })`
  in a task that must wait for one;
- `serial.onData(fn)`, `usbSerial.onData(fn)` for bytes that arrive;
- a promise for an operation the program starts (`await device.read(2)` of `device/i2c`,
  `await led.show()` of `stdlib/rgbled`);
- a task fed by callbacks waits on a [mailbox](../tasks/#values-from-a-callback-a-mailbox):
  it wakes up once per value, never in vain.

## Poll slowly, and only what needs it

What cannot raise an event (an analog input, a sensor on I2C, an animation) is polled:
at the slowest rate that is still right.

| What | Enough |
|---|---|
| an animation of LEDs (breathing, blinking) | 25 Hz (`self.every(40)`) |
| a value shown to a person | 5 to 10 Hz |
| a temperature, a humidity, a battery voltage | 1 Hz or less |

Then do nothing when nothing changed: a strip that shows a fixed color is not sent again
at each step, a value equal to the last one is not sent on a link.

```js
let shown = null;                              // the color on the strip now

async function showColor(color) {
    if (color === shown) {
        return;                                // nothing to send
    }
    led.fill(...color.rgb);
    await led.show();
    shown = color;
}
```

## Animations from the clock

A step of a ticker may be late: the steps passed are skipped (see
[Small tasks](../tasks/#a-task)). An animation that moves one step per turn of its loop
then slows down when the board is busy. Compute it from the time instead: it stays at
the right speed, and its rate can be lowered without changing its look.

```js
import { task } from "stdlib/tasks";
import { RgbLed } from "stdlib/rgbled";

const led = new RgbLed(48);
const BREATH = 3000;                           // one breath, in milliseconds

task("breath", async self => {
    const start = Date.now();
    const tick = self.every(40);               // 25 Hz
    while (!self.stopped) {
        const phase = ((Date.now() - start) % BREATH) / BREATH;
        const level = (1 - Math.cos(2 * Math.PI * phase)) / 2;
        led.set(0, 0, Math.round(40 * level), Math.round(20 * level));
        await led.show();
        await tick.next();
    }
});
```

On a board, `Date.now()` counts from the boot: only differences of it mean something.

## Measure

`load()` of `device/board` gives the share of the last second the JS thread worked
(`js`, 0 to 1) and its peak (`jsPeak`); `memory()` the JS heap now, its peak and its
size (see [Memory](../devices/#memory)). `qjsx device info` shows the same figures from
the PC, while the program runs:

```text
JS heap:  180.4 KB used, 180.9 KB at most (this run), of 248.0 KB
JS load:  20.5 % (last second), 34.0 % at most (this run)
```

A program can report them itself, rarely:

```js
import { task } from "stdlib/tasks";
import { load, memory } from "device/board";

task("health", async self => {
    while (!self.stopped) {
        await self.sleep(10000);
        const { js, jsPeak } = load();
        const { jsHeap, jsHeapPeak } = memory();
        console.log(`load ${Math.round(js * 100)} % (peak ${Math.round(jsPeak * 100)} %), `
            + `heap ${jsHeap} bytes (peak ${jsHeapPeak})`);
    }
});
```

Measure on the board: the twin runs at the speed of the PC, and its JS heap is about
twice as large (values and pointers are twice as wide on the PC). The twin still tells
which of two versions of a program uses less.

A load that stays above about 50 % leaves little room for the callbacks: poll less,
or move work out of the loops.

To see which functions take that time, `qjsx profile` samples the JS thread of the device
for a while (`--duration`, 5 s by default; `--interval` between two samples, 1 ms) and
prints the functions that take its time, their own and with what they call (`--top`, 20
by default), named from the debug files of the project's package. The desktop agent and
the twin have it; the firmware of a board must be built with the `QJSX_PROFILER` option
(off by default), else the device answers an error.

## Memory

The details are in [Programs on a device](../devices/#memory). In short:

- a `const` of a module holding a literal number or string costs nothing (see
  [Constants cost nothing](../devices/#constants-cost-nothing)): name values freely;
- the bytecode and the names of the program stay in the flash: a larger program costs
  flash much more than RAM;
- each function of a loaded module costs RAM, called or not: `qjsx build` leaves out the
  declarations nothing uses (see
  [What the build leaves out](../devices/#what-the-build-leaves-out-what-it-refuses));
- each function made is an object in the heap: make functions once, at the top of a
  module or of a task, not in a loop or at each call;
- each async function that waits (a task, a loop that awaits) keeps its frame in the
  heap: a few long-lived tasks, not one per event;
- an object that refers to itself (a timer callback that clears its own timer, a
  callback that removes itself from a set) is freed only by the cycle collector, later:
  made at a high rate, such cycles fill a small heap meanwhile. `stdlib/tasks` makes
  none.

```js
// not: a new function at each value received
port.onData(bytes => {
    bytes.forEach(byte => parser.push(byte));
});

// a plain loop makes nothing
port.onData(bytes => {
    for (const byte of bytes) {
        parser.push(byte);
    }
});
```

An allocation that does not fit throws `InternalError: out of memory` where it was
asked; the agent of the board still answers (`qjsx device info`, `qjsx deploy`).

## Tasks

[Small tasks](../tasks/) are the way to write what a board does all the time. Three
rules:

- the loop of a task is `while (!self.stopped)`: `stop()` then ends it at its next wait,
  and the code after the loop cleans up (a LED off, a port closed);
- an error that leaves the body ends the task: a loop that must survive a failed step (a
  sensor that does not answer once) catches the errors of that step itself;
- several tasks that use the same bus or port give their sequences of transfers to one
  `Mutex` (`await bus.withLock(async () => { ... })`), so that nobody comes between two
  transfers of one command. A job never calls `withLock` on its own mutex.

```js
import { task, Mutex } from "stdlib/tasks";
import { I2c } from "device/i2c";

const bus = new I2c(0, { sda: 8, scl: 9 });
const sensor = bus.device(0x48);
const busLock = new Mutex();                   // every task that uses the bus goes through it

task("sensor", async self => {
    const tick = self.every(1000);
    while (!self.stopped) {
        try {
            const raw = await busLock.withLock(() => sensor.writeRead([0x00], 2));
            report(raw);
        } catch (error) {
            console.error("sensor:", error);   // printed, the loop goes on
        }
        await tick.next();
    }
});
```

A synchronous transfer (`writeReadSync()` of `device/i2c`) blocks the JS thread for its
duration; a promise costs a few hundred microseconds of the thread. For a few bytes the
synchronous one is often the cheaper; `load()` tells.

## Errors

| Where | What happens |
|---|---|
| the body of a task | printed as `<name>: ended by an error: <error>`, the task is over, `join()` is rejected |
| a callback (`onChange`, `onData`, a timer) | printed, the program goes on |
| a promise rejected that nobody awaits or catches | printed as `Uncaught (in promise)`, the program goes on |
| `process.exit(code)` | the program ends; the firmware and its agent stay |

A program that must never stop catches what it can recover from, and lets the rest be
printed: `qjsx monitor` shows it.

## No compiler on the device

`eval(...)`, `Function(...)` and `new Function(...)` throw on a device, and `import()`
is refused when the package is built: static `import` only (see
[Programs on a device](../devices/#no-compiler-on-the-device)). Data received from a
link is parsed (`JSON.parse`), never evaluated.

## The stack

A JS call takes about 360 bytes of the stack of the JS task: a 32 KB stack (a board
without PSRAM) holds about 66 nested calls, a 48 KB one about 110. Deeper, the engine
throws a `RangeError`. Prefer loops to recursion over data whose depth you do not
control (a tree received from a link); `JSON.parse` itself holds about 270 levels of
nested arrays with 32 KB.

## Checklist

- events (`onChange`, `onData`, a mailbox) rather than a loop that looks;
- loops at the slowest rate that is right; nothing sent when nothing changed;
- animations computed from `Date.now()`;
- functions made once; few long-lived tasks;
- each loop `while (!self.stopped)`, its fallible steps in `try` / `catch`;
- one `Mutex` per shared bus;
- `load()` and `memory()` checked on the board, not only on the twin.
