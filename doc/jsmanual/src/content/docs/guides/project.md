---
title: The project file
description: qjsx.json, at the root of a project.
---

`qjsx.json` sits at the root of a project. Every key is optional; an unknown key, or one
of the wrong type, is an error.

```json
{
    "name": "weather",
    "main": "src/main.js",
    "sources": ["src/**/*.js"],
    "exclude": ["src/**/*.test.js"],
    "output": "build/packages",
    "keep": 10,
    "target": "serial:COM11",
    "debugPort": 9339,
    "env": { "LOG": "debug" },
    "libraries": ["libs/sensors.qjslib"]
}
```

| Key | Default | Role |
|---|---|---|
| `name` | the folder's name | name of the package (letters, digits, `.`, `_`, `-`) |
| `main` | `"src/main.js"` | the module run at start |
| `sources` | `["src/**/*.js"]` | the modules compiled into the package (globs `*`, `?`, `**/`) |
| `exclude` | `[]` | patterns removed from `sources` |
| `output` | `"build/packages"` | where the packages are written |
| `keep` | `10` | how many packages of the project are kept |
| `target` | — | the device of `deploy`, `debug`, `monitor`, `device` when `--target` is not given |
| `debugPort` | `9339` | the port of `qjsx debug` |
| `env` | — | `process.env` of the program on a device (strings only) |
| `libraries` | `[]` | `.qjslib` files the program may import, besides the standard library |
| `desktop` | — | the program as a program of the PC: root folder and permissions ([Programs of the PC](/guides/pc-programs/)) |
| `twin` | — | the digital twin of the project: board, parts, ports ([The digital twin](/guides/twin/)) |

## Targets

A target names a device:

| Target | Device |
|---|---|
| `serial:COM11`, `serial:/dev/ttyUSB0@921600` | a board on a serial port (USB to serial bridge) |
| `tcp:esp.local:5555` | an agent on the network |
| `twin` | the digital twin of the project on this PC (`qjsx agent` in its folder) |

`qjsx targets` lists the serial ports.

## Module names

| Name | Module |
|---|---|
| `./util.js`, `../lib/x.js` | a module of the program, by its path |
| `sys/fs`, `sys/crypto`... | a native module of the system |
| `device/gpio`, `device/i2c`... | a native module of the hardware the program runs on |
| `stdlib/path`, `stdlib/rgbled`... | a module of the standard library |
| `sensors/bme280` | a module of a library declared in `libraries` |

A bare name (`fs`, `path`) is an error: qjsx has no Node module names.
