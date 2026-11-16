---
title: Programs on a device
description: What a program meets on a board, and what it cannot do there.
---

A device (an ESP32 or RP2350 board today) runs the qjsx firmware: the QuickJS engine
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

export function wait() {
    return sleep(DELAY);     // compiled as sleep(5000)
}
```

Such a constant takes no memory on the device, so naming values costs nothing. Two
things follow: reading it before its declaration ran gives its value (JavaScript throws
a ReferenceError there), and a breakpoint set on its line stops at the next statement.
The debugger still shows it, with its value. An exported constant, a `const` holding an
object, an array or the result of a call, and a `let` stay ordinary variables.

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
| `process.platform` | `"esp32"`, `"rp2350"` (the twin: the platform of its board) |
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

A board without PSRAM gives the program its internal RAM only: a few hundred kilobytes,
shared with the firmware. `qjsx device info` shows what the program's JS heap uses now
and the most it used in this run (or the last one), and the free memory of the board:

```text
JS heap:  182.4 KB used, 197.0 KB at most (this run)
RAM:      61.3 KB free, largest block 31.7 KB, lowest 40.2 KB since the boot
```

The digital twin shows the JS heap too, but measured on the PC: values and pointers
take twice the room there, so the figure is larger than on the board. It is still useful
to compare two versions of a program.

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
storage. (The RP2350 has no file system yet.)

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
[The project file](/guides/project/)): files of the same name are replaced, the others
(what the program wrote itself) are kept. A file is written to a temporary file then put
in place: a cut transfer never leaves half a file.
