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

## Uncaught exceptions

An exception nobody catches in a callback is printed and the program goes on.

## Native modules of a board

The firmware of each board has its own list of native modules; `qjsx deploy` refuses a
program that imports a module the firmware does not have. The digital twin offers exactly
the modules of the board it plays.

## Files

Files on the boards (a file system in the `data` partition, mounted on `/data`) are not
there yet: on a board, `sys/fs` fails today. The desktop agent and the digital twin keep
the files of the program in a `data` folder of their storage, and a program cannot reach
anything outside it.
