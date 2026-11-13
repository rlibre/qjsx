---
title: Programs of the PC
description: qjsx run, standalone executables, permissions.
---

qjsx also runs programs on the PC (Windows, macOS, Linux): scripts with `qjsx run`, or a
project delivered as a standalone executable with `qjsx build --executable`. A program of
the PC reaches nothing it was not granted, on the model of Deno: a bug in a script must
not be able to wipe a disk.

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

## Standalone executables

```bash
qjsx build --executable
```

writes `build/packages/<name>.exe` (or `<name>`): a small runner with the package inside.
Its permissions come from the `desktop` section of `qjsx.json`, baked in:

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
- Variables: `${executableFolder}`, `${userAppDataFolder}` (`%APPDATA%/<name>`,
  `~/Library/Application Support/<name>`, `~/.local/share/<name>`), `${currentFolder}`,
  `${rootFolder}`.
- `process.cwd()` is the root folder: relative paths of `sys/fs` start there.
- The only option of an executable is `--qjsx-root-folder=<folder>` (or the variable
  `QJSX_ROOT_FOLDER`): two copies of a program can work on two folders.
- Paths are compared as real paths (links resolved).
