# qjsx: context for an AI assistant writing qjsx programs

Give this file to an assistant that writes JavaScript programs for qjsx (not the qjsx
engine itself). It is short on purpose: the exact API is in the typings.

## What qjsx is

- A JavaScript runtime built on QuickJS (modern ECMAScript: modules, classes, async /
  await, generators, BigInt, typed arrays). One executable, `qjsx`, on the PC; a firmware per
  board on the devices (ESP32 boards, RP2350); a digital twin that plays a board on the PC.
- A device has no compiler: `qjsx build` compiles the program on the PC into a package
  (`.qjspkg`, bytecode), `qjsx deploy` sends it to the device, which runs it.
- One JS thread and one event loop per program. Hardware events call callbacks;
  operations started by the program return promises.
- It is NOT Node.js and NOT a browser.

## The project

```
project/
├── qjsx.json
└── src/
    └── main.js
```

`qjsx.json`, every key optional:

```json
{
    "name": "blink",
    "main": "src/main.js",
    "sources": ["src/**/*.js"],
    "target": "serial:COM11",
    "env": { "LOG": "debug" },
    "libraries": ["libs/sensors.qjslib"],
    "data": "data/",
    "twin": { "board": "yd-esp32-s3" }
}
```

Targets: `serial:COM11`, `serial:/dev/ttyUSB0@921600`, `tcp:host:5555`, `twin` (the twin
of the project, started by `qjsx agent` in its folder).

## Commands

| Command | Does |
|---|---|
| `qjsx build` | compiles the project into `build/packages/<name>-<id>.qjspkg`; writes the typings in `.qjsx/types/` and a `jsconfig.json` if the project has none |
| `qjsx deploy [--target t] [--monitor] [--ram] [--reset] [--data]` | sends the latest package to the device and starts it (`--ram`: not written to flash; `--data`: also the files of the `data` folder) |
| `qjsx monitor` | shows the output of the program on the device |
| `qjsx debug [--restart]` | debugger server for VS Code (`node` attach, port 9339) |
| `qjsx agent` | in a project folder with a `twin` section: the digital twin, page on http://127.0.0.1:8090 |
| `qjsx device info` | firmware, package, JS heap, JS load, free RAM |
| `qjsx device ls / read / write / rm / mkdir / mv` | files of the device's data folder (`/data`) |
| `qjsx device run / stop / reset` | starts, stops the program; restarts the device |
| `qjsx run [-A] script.js` | runs a script on the PC (Deno-like permissions: `-R`, `-W`, `-N`, `-E`, `--allow-serial`) |
| `qjsx mklib` | makes a `.qjslib` library from a library project |

## Modules

Imported by full name only; a bare name (`fs`, `path`) is an error.

| Name | Module | Where |
|---|---|---|
| `sys/fs` | files, synchronous only (`readFileSync`, `writeFileSync`, `existsSync`, `statSync`, `readdirSync`, `mkdirSync`, `rmSync`, `renameSync`, `unlinkSync`) | everywhere; a device: confined to `/data` |
| `sys/crypto` | `sha256`, `sha1`, `randomBytes` | everywhere |
| `sys/net` | TCP: `listen`, `connect`, `Connection` | PC only |
| `device/board` | `id`, `mac`, `restart`, `memory`, `load` | ESP32, twin |
| `device/gpio` | `Gpio`: `read`, `write`, `toggle`, `onChange`, `waitFor`, `close` | ESP32, RP2350, twin |
| `device/pwm` | `Pwm`: `setDuty`, `setFrequency`, `setPulseWidth` | ESP32, twin |
| `device/adc` | `Adc`: `read`, `readVoltage` | ESP32, twin |
| `device/i2c` | `I2c`, `bus.device(address)` -> `I2cDevice`: `write`, `read`, `writeRead` (promises) and `...Sync` | ESP32, twin |
| `device/rmt` | `Rmt`: `send`, `sendBytes` | ESP32, twin |
| `device/serial` | `Serial`: `onData`, `onClose`, `write`, `close` (a UART number + pins on a board, a port name on the PC) | ESP32, PC, twin |
| `device/usb` | `Usb`, `UsbCdc` (the board as a USB device) | ESP32-S2/S3/P4, twin |
| `stdlib/tasks` | `task`, `Task` (`stopped`, `stop`, `join`, `sleep`, `every`, `mailbox`), `Mutex.withLock` | everywhere |
| `stdlib/rgbled` | `RgbLed` (WS2812): `set`, `fill`, `setHsv`, `fillHsv`, `show` | ESP32, twin |
| `stdlib/usb` | `UsbSerial` (one USB serial function) | ESP32-S2/S3/P4, twin |
| `stdlib/path` | as Node's `path` | everywhere |
| `<library>/<module>` | a module of a `.qjslib` declared in `libraries` | |

Globals: `console` (`log`, `info`, `debug`, `warn`, `error`), `setTimeout`,
`setInterval`, `clearTimeout`, `clearInterval`, `TextEncoder`, `TextDecoder`, `crypto`
(`getRandomValues`, `randomUUID`), `process` (`platform`, `versions`, `argv`, `env`,
`exit`, `cwd`, `stdout.write`, `stderr.write`), `import.meta.url` / `import.meta.main`.
Bytes are `Uint8Array` (`toHex()`, `toBase64()`, `Uint8Array.fromBase64()` are there).

**The exact API** (every option, error and default) is in the typings: after `qjsx
build`, `<project>/.qjsx/types/index.d.ts`. Read them before using a module; never guess
an option name (it is `"openDrain"`, not `"open-drain"`). Errors of the system carry a
Node-like `code` (`ENOENT`, `EBUSY`, `ETIMEDOUT`...).

## What does NOT exist

- Node: `require`, `module.exports`, `Buffer`, `fs.promises` and async `fs`, `events` /
  `EventEmitter`, `node:*` modules, `__dirname`, `process.nextTick`, `process.on(...)`.
- Browser: DOM, `window`, `fetch`, `XMLHttpRequest`, `WebSocket`, `localStorage`,
  `performance`, `queueMicrotask`, `structuredClone`, `requestAnimationFrame`.
- On a device: `eval`, `Function(...)`, `new Function`, dynamic `import()` (the build
  refuses it); `sys/net`, `sys/compiler`. Time: `Date.now()` counts from the boot (no
  wall clock).

## Rules for a program that fits a board

A board runs JS about 100 times slower than a PC (an ESP32-S3: `await null` 200-300 µs,
a 10 ms loop doing nothing takes 20-35 % of the JS thread) and may have only about 250 KB
of JS heap.

1. Events, not polling: `gpio.onChange`, `serial.onData`, a task waiting on a
   `mailbox` fed by callbacks.
2. Poll only what cannot raise an event, slowly (25 Hz for an animation, 1 Hz for a
   sensor); do nothing when nothing changed.
3. Animations computed from `Date.now()`, not from a count of steps.
4. Memory: `const` literals of a module cost nothing; make functions once (not in loops
   or per event); few long-lived tasks; no self-referencing objects made at a high rate.
5. Tasks: loop on `while (!self.stopped)`, wait with `await tick.next()` /
   `self.sleep()`; catch the errors of a step that may fail (an error leaving the body
   ends the task); share a bus between tasks through one `Mutex` (`withLock`), never
   nested on the same mutex.
6. Errors: an error in a callback is printed and the program goes on; a rejection
   nobody handles is printed as `Uncaught (in promise)`.
7. Stack: about 360 bytes per JS call (66 nested calls with a 32 KB stack): avoid deep
   recursion.
8. Measure on the board with `load()` and `memory()` of `device/board`, or
   `qjsx device info`; the twin runs at PC speed.

## Example

A LED that blinks, a button that pauses it, a sensor read every second; the I2C bus is
shared through a mutex.

```js
import { task, Mutex } from "stdlib/tasks";
import { Gpio } from "device/gpio";
import { I2c } from "device/i2c";

const led = new Gpio(5, { mode: "output" });
const button = new Gpio(0, { mode: "input", pull: "up" });
const bus = new I2c(0, { sda: 8, scl: 9 });
const sensor = bus.device(0x48);
const busLock = new Mutex();

let paused = false;
button.onChange(level => {
    if (level === 0) {
        paused = !paused;
    }
}, { edge: "falling", debounce: 20 });

task("blink", async self => {
    const tick = self.every(500);
    while (!self.stopped) {
        if (!paused) {
            led.toggle();
        }
        await tick.next();
    }
    led.write(0);
});

task("sensor", async self => {
    const tick = self.every(1000);
    while (!self.stopped) {
        try {
            const raw = await busLock.withLock(() => sensor.writeRead([0x00], 2));
            console.log(`raw ${(raw[0] << 8) | raw[1]}`);
        } catch (error) {
            console.error("sensor:", error);
        }
        await tick.next();
    }
});
```

The manual: `doc/jsmanual/` (guides: getting started, the project file, programs on a
device, small tasks, good practices, the twin, debugging, libraries, programs of the PC).
