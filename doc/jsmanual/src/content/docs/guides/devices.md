---
title: Programs on a device
description: What a program meets on a board, and what it cannot do there.
---

A device (an ESP32, Teensy 4.0 or RP2350 board today) runs the qjsx firmware: the QuickJS engine
without its compiler, the native modules of the board and an agent that talks to the PC.
The program arrives as a package of bytecode, compiled on the PC by `qjsx build`.

## No compiler on the device

- `eval(...)`, `Function(...)` and `new Function(...)` throw on a device. `qjsx build`
  warns about each direct use, with its position (`file:line:column: warning: ...`), but
  the build succeeds.
- `import()` (dynamic import) is refused by the compiler: every module of the program is
  known when the package is built, and the library modules it imports are copied into the
  package. Static `import` only.

## Constants cost nothing

`qjsx build` computes the expressions made of literal numbers and strings (`5 * 1000`,
`1 << 4`, `"pin" + "-" + 4`), and replaces a `const` of a module that has such a value by
that value wherever the module reads it:

```js
const DELAY = 5 * 1000;      // no variable is made for it
const LABEL = "led-" + 2;

export function later(job) {
    return setTimeout(job, DELAY);     // compiled as setTimeout(job, 5000)
}
```

Such a constant takes no memory on the device, so naming values costs nothing. Two
things follow: reading it before its declaration ran gives its value (JavaScript throws
a ReferenceError there), and a breakpoint set on its line stops at the next statement.
The debugger still shows it, with its value. An exported constant is replaced the same
way inside its module, but keeps its variable for the modules that import it. A `const`
holding an object, an array or the result of a call, and a `let`, stay ordinary
variables.

## What the build leaves out, what it refuses

On a device every function costs RAM as soon as its module is loaded, called or not
(about 275 bytes on a 32-bit board). `qjsx build` therefore leaves out of the package the
top-level declarations nothing uses (functions, classes, constants, in the program and in
the library modules it imports), and says which:

```text
qjsx: stdlib/tasks: left out, not used: Mutex
```

What is used is followed from the code that runs: the statements of each module that do
something, the names they use, the imports and re-exports (`import * as ns` uses every
export of the module). A module that calls `eval()` keeps everything. The package keeps
the original sources: the debugger shows them, at the same positions.

An import of a name the other module does not export is a build error, with its
position, instead of a program the device refuses when it starts:

```text
qjsx: src/main.js:3:10: 'sleep' is not exported by stdlib/tasks
```

## One thread, one loop

A program runs on one JS thread. Hardware events (a pin changes, bytes arrive, a send
ends) are posted to its event loop and delivered between two pieces of JS:

- spontaneous events call **callbacks**: `gpio.onChange(fn)`, `serial.onData(fn)`;
- operations the program starts return **promises**: `await bus.device(0x76).read(2)`;
- a module may offer both.

An object that waits for events (a pin with `onChange`, an open serial port, a started
USB device) keeps the program running until its `close()`.

A long computation does not stop the board: the runtime gives the other tasks of its core
a tick every 100 ms. An endless JS loop keeps running, but the debugger, `qjsx device
stop` and `qjsx deploy` still reach it.

## process on a device

| | On a device |
|---|---|
| `process.argv` | `["qjsx", "<main module>"]` |
| `process.env` | the `env` object of `qjsx.json`, `{}` without one; never the PC's environment |
| `process.platform` | `"esp32"`, `"imxrt1062"` (Teensy 4.0), `"rp2350"` (the twin: the platform of its board) |
| `process.cwd()` | the data folder of the device |
| `process.exit(code)` | ends the program, not the firmware: the agent stays, `qjsx deploy` or a restart starts a program again |
| `process.versions` | `{ qjsx, quickjs, build }`: `build` is the build id of the package |

## Random numbers

`crypto.getRandomValues()` and `crypto.randomUUID()` (globals, as on the web) and
`randomBytes()` of `sys/crypto` give random bytes of cryptographic quality. On an ESP32 the
generator is seeded at boot from the entropy of the chip (it borrows the ADC for a moment,
before any program runs). `Math.random()` is seeded from the same source at each run of
the program: it is not the same sequence after each boot.

## Uncaught exceptions

An exception nobody catches in a callback is printed and the program goes on.

## Memory

### The JS heap

A board with PSRAM puts the JS heap there: several megabytes. A board without PSRAM gives
the program its internal RAM only: when the first program starts, the firmware reserves
one region of it for the JS heap (the largest free block, less 16 KB it keeps for itself),
about 250 KB on an ESP32-S3 (the Teensy 4.0, which has no PSRAM either, gives it a fixed
512 KB region). `qjsx device info` shows what the program's JS heap uses now,
the most it used in this run (or the last one), the size of that region, and the free
memory left to the firmware:

```text
JS heap:  180.4 KB used, 180.9 KB at most (this run), of 248.0 KB
RAM:      46.5 KB free, largest block 31.0 KB, lowest 44.5 KB since the boot
```

The program reads the same figures itself with `memory()` of `device/board`, and the
load of its JS thread with `load()`: the share of the last second it worked rather than
waited for an event, its highest value over a full second, and the milliseconds worked
and elapsed, to compute it over any time.

```js
import { memory, load } from "device/board";

const { jsHeap, jsHeapSize } = memory();
console.log(`heap ${jsHeap} of ${jsHeapSize} bytes, load ${Math.round(load().js * 100)} %`);
```

The digital twin shows the JS heap too, but measured on the PC: values and pointers
take twice the room there, so the figure is larger than on the board. It is still useful
to compare two versions of a program. The twin does not limit the heap either: a program
that runs out of memory on a board may run on the twin.

### The code stays in flash

`qjsx build` links the modules of the program into one package: their bytecode and the
names they use (of functions, properties, variables) are read in place from the flash of
the board, not copied into the JS heap. The heap holds what the program makes as it runs:
its objects, arrays and strings, its functions and closures, the async functions that
wait (a task, a loop that awaits), and some tables of each function. A larger program
costs flash much more than RAM.

What saves RAM:

- constants of a module (see [Constants cost nothing](#constants-cost-nothing));
- functions made once, not in a loop or at each call (each closure is an object);
- few long-lived waiting async functions: each one keeps its frame;
- no function that refers to itself made over and over (a timer that clears itself, a
  callback that removes itself from a set): such a cycle is only freed by the cycle
  collector, later, and fills a small heap meanwhile. `stdlib/tasks` avoids it.

### When memory runs out

An allocation that does not fit throws `InternalError: out of memory` in the code that
asked for it; a task stops with it, a callback prints it and the program goes on. The
agent of the board does not use the JS heap: `qjsx deploy`, `qjsx device info` and the
debugger still reach the board.

### The stack

Each board gives the JS task a stack of its own size (48 KB on most ESP32 boards, 32 KB
on a board without PSRAM that needs the RAM). A JS call takes about 360 bytes of it: a
32 KB stack holds about 66 nested calls, and `JSON.parse` about 270 levels of nested
arrays. Deeper, the engine throws a `RangeError` instead of crashing the board.

## Native modules of a board

The firmware of each board has its own list of native modules; `qjsx deploy` refuses a
program that imports a module the firmware does not have. The digital twin offers exactly
the modules of the board it plays.

## Files

An ESP32 board keeps the files of its programs in its `data` partition (LittleFS),
mounted on `/data`: `process.cwd()` is `/data`, relative paths of `sys/fs` start there,
and a program cannot reach anything outside it. LittleFS commits a file when it is closed:
a power cut during `writeFileSync()` keeps the old content whole. The partition is
formatted the first time, and kept when a program is deployed or the firmware flashed.
The desktop agent and the digital twin keep these files in a `data` folder of their
storage. (The Teensy 4.0 and the RP2350 have no file system yet: `sys/fs` throws
`ENOSYS` there.)

From the PC:

```bash
qjsx device ls [folder]
qjsx device read <file> [local]     # no local: shown
qjsx device write <local> [file]
qjsx device rm <file | empty folder>
qjsx device mkdir <folder>
qjsx device mv <from> <to>          # a file replaces a file; folders never replace
qjsx device info                    # also: the space used in /data, the memory
```

`qjsx deploy --data` also writes the files of the project's `"data"` folder (see
[The project file](../project/)): files of the same name are replaced, the others
(what the program wrote itself) are kept. A file is written to a temporary file then put
in place: a cut transfer never leaves half a file.

## SPI

`device/spi` drives SPI chips: displays, flash memories, sensors, converters. A bus is a
controller of the chip on its pins (port 2 or 3 on the ESP32); each chip on it is a
device with its chip select, clock, mode and, for a display, its data / command (DC) pin.
A transaction holds the chip select from its first segment to the last, and the driver
sets the DC pin just before each segment: a display needs no `Gpio` of its own.

```js
import { Spi } from "device/spi";

const bus = new Spi(2, { sclk: 12, mosi: 11 });
const lcd = bus.device({ cs: 10, dc: 9, frequency: 20000000 });

// waking up: a command, 120 ms, the next one
await lcd.command([0x01]);                                  // SWRESET
await new Promise((resolve) => setTimeout(resolve, 120));
await lcd.command([0x11]);                                  // SLPOUT

// a window of 128 x 160 pixels, then its pixels, as an ST7735 wants them:
// each command with DC low, its parameters and the pixels with DC high
const pixels = new Uint8Array(128 * 160 * 2).fill(0xff);   // white, RGB565
await lcd.transaction([
    { write: [0x2a], dc: 0 }, { write: [0, 0, 0, 127] },   // CASET: columns 0..127
    { write: [0x2b], dc: 0 }, { write: [0, 0, 0, 159] },   // RASET: rows 0..159
    { write: [0x2c], dc: 0 }, { write: pixels },           // RAMWR, then the pixels
]);
```

Transactions run one at a time per bus, in the order of the calls. The promise forms
(`transaction`, `write`, `read`, `transfer`, `writeRead`, `command`) copy the bytes they
send; the `Sync` forms block the JS thread and send the bytes in place, which spares a copy
of a large frame on a board with little RAM. A promise form never throws: a wrong argument
rejects its promise too. A `delay` segment waits at most 1 ms with the chip select held;
a longer wait is an `await` between two transactions.

## DAC

`device/dac` drives a pin with a voltage, through the DAC (digital to analog converter)
of the chip. Only the original ESP32 (GPIO25 and GPIO26) and the ESP32-S2 (GPIO17 and
GPIO18) have one, of 8 bits: 256 codes from 0 V to the full scale (3.3 V), a step of
about 13 mV. The ESP32-S3, the RP2350 and the Teensy 4.0 have none: there, a `device/pwm`
output behind an RC filter (a resistor, then a capacitor to the ground) makes a voltage
(this module does not do it for you).

```js
import { Dac } from "device/dac";

const out = new Dac(25);              // GPIO25, at 0 V
out.writeVoltage(1.2);                // the nearest code: 93, about 1.203 V
out.write(255);                       // a raw code: the full scale
console.log(out.bits, out.fullScale, out.value);   // 8 3.3 255
out.close();
```

Everything is synchronous. A code must be an integer of the resolution and a voltage
within 0 and `fullScale`: anything else is a `RangeError`, never rounded silently into
range. Like a `Pwm`, a `Dac` stops its output when it is closed AND at the end of the
program: the pin is given back. In the digital twin (board `esp32-devkitc-wroom32`), the
parts wired on the pin read its voltage, and the page shows it ("dac 1.20 V").
