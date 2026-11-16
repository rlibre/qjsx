---
title: Programs of the PC
description: qjsx run, permissions.
---

qjsx also runs programs on the PC (Windows, macOS, Linux) with `qjsx run`: a script, or the
package of a project. A program of the PC reaches nothing it was not granted, on the model
of Deno: a bug in a script must not be able to wipe a disk.

## qjsx run

```bash
qjsx run [options] <script.js | package.qjspkg> [arguments of the program]
```

| Option | Grants |
|---|---|
| `-A`, `--allow-all` | everything |
| `-R`, `--allow-read[=folder,...]` | reading files (everywhere without a list) |
| `-W`, `--allow-write[=folder,...]` | writing files |
| `-N`, `--allow-net[=host[:port],...]` | the network |
| `-E`, `--allow-env[=NAME,...]` | environment variables |
| `--allow-serial[=port,...]` | serial ports |
| `--qjsx-root-folder=<folder>` | the program's `process.cwd()` |

Options before the script are qjsx's; after it, they belong to the program
(`process.argv`).

## Permissions of a package

A package (`qjsx build`, then `qjsx run build/packages/<name>-<build>.qjspkg`) also gets
the `desktop` section of `qjsx.json`, copied into it by the build:

```json
"desktop": {
    "rootFolder": "${userAppDataFolder}",
    "permissions": {
        "read": ["${rootFolder}"],
        "write": ["${rootFolder}"],
        "net": ["api.example.com:443"],
        "env": ["HOME"],
        "serial": ["COM3"]
    }
}
```

- Nothing is allowed by default. A `rootFolder` without `permissions` may be read, not
  written. A root at the root of a disk is refused.
- Variables: `${executableFolder}` (the folder of the package file),
  `${userAppDataFolder}` (`%APPDATA%/<name>`, `~/Library/Application Support/<name>`,
  `~/.local/share/<name>`), `${currentFolder}`, `${rootFolder}`.
- `process.cwd()` is the root folder: relative paths of `sys/fs` start there.
- `--qjsx-root-folder=<folder>` (or the variable `QJSX_ROOT_FOLDER`) replaces the root
  folder: two copies of a program can work on two folders.
- Paths are compared as real paths (links resolved).
