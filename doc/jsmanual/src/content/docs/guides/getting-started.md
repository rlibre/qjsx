---
title: Getting started
description: Create a qjsx project, run it on the PC, on the digital twin of a board, then on the board.
---

qjsx is one executable, `qjsx`, that does everything on the PC: it runs scripts, compiles
projects into packages, sends them to a device, plays the digital twin of a board and
serves the debugger to VS Code. A device runs the qjsx firmware of its board.

## A first script

```js
// hello.js
console.log(`hello from ${process.platform}`);
setTimeout(() => console.log("one second later"), 1000);
```

```bash
qjsx run hello.js
```

`qjsx run` runs a script on the PC. It reaches nothing but what you grant, as Deno does:
no file, network, environment variable or serial port without `-R`, `-W`, `-N`, `-E`,
`--allow-serial`, or `-A` for everything (see [Programs of the PC](/guides/pc-programs/)).

## A project

A project is a folder with a `qjsx.json` file and its sources:

```
blink/
├── qjsx.json
└── src/
    └── main.js
```

```json
{
    "name": "blink",
    "target": "twin",
    "twin": { "board": "yd-esp32-s3" }
}
```

```js
// src/main.js
import { Gpio } from "device/gpio";

const led = new Gpio(5, { mode: "output" });
setInterval(() => led.toggle(), 500);
```

Every key of `qjsx.json` is optional: `main` is `src/main.js`, the sources are
`src/**/*.js` (see [The project file](/guides/project/)).

## Build

```bash
qjsx build
```

`qjsx build` compiles every module of the program, and the library modules it imports,
into a package: `build/packages/blink-<build id>.qjspkg`. A device has no compiler: it
runs the bytecode of the package.

The build also writes the typings of qjsx in the project (`.qjsx/types/index.d.ts`), and
a `jsconfig.json` when the project has none: VS Code then completes and checks
`import { Gpio } from "device/gpio"`, the options of each class and the globals
(`process`, `console`...). Leave `.qjsx/` out of git: each build writes it again.

A project that has its own `jsconfig.json` or `tsconfig.json` must name the folder, the
build says so when it does not:

```json
{
    "compilerOptions": {
        "types": ["./.qjsx/types"]
    }
}
```

## Run it on the twin

The digital twin plays the board on the PC. In the folder of the project:

```bash
qjsx agent
```

It reads the `"twin"` section of `qjsx.json` and opens the page of the twin on
http://127.0.0.1:8090: pins, LEDs, buttons, buses, UARTs and an analyzer. In another
terminal:

```bash
qjsx deploy --monitor
```

`"target": "twin"` sends the package to the twin of the project; `--monitor` shows the
output of the program. See [The digital twin](/guides/twin/).

## Run it on the board

Flash the qjsx firmware of the board once, then give the serial port of the board as the
target:

```bash
qjsx targets
qjsx deploy --target serial:COM11 --monitor
```

`qjsx deploy` installs the package in the flash of the board and starts it; the program
starts again at each power on. `--ram` loads it in RAM only, for a quick try.

## Debug it

```bash
qjsx debug
```

then attach VS Code to port 9339: see [Debugging in VS Code](/guides/debugging/).
