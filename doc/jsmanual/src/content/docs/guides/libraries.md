---
title: Libraries
description: Share JS modules between programs with .qjslib files.
---

A library is a set of JS modules delivered as one `.qjslib` file. Its modules are imported
as `<library>/<module>`: the standard library is `stdlib` (`stdlib/path`,
`stdlib/rgbled`, `stdlib/usb`).

## Make a library

A library project has `modules` instead of `main` and `sources`, and a version:

```json
{
    "name": "sensors",
    "version": "1.0.0",
    "modules": {
        "bme280": "src/bme280.js",
        "filter": "src/filter.js"
    }
}
```

```bash
qjsx mklib
```

writes `build/packages/sensors.qjslib`. It holds the sources and their bytecode.

## Use it

```json
{
    "libraries": ["libs/sensors.qjslib"]
}
```

```js
import { read } from "sensors/bme280";
```

`qjsx build` copies into the package every library module the program imports, whole,
and those they import in turn. A library made by another version of the compiler is
compiled again from its sources (the build says so).

The names `sys/...` and `device/...` are reserved for the native modules: no library may
take them.
