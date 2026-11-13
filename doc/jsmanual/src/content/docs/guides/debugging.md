---
title: Debugging in VS Code
description: Breakpoints, stepping and variables on the board, from VS Code.
---

`qjsx debug` is a debugger server for VS Code: it speaks the protocol of VS Code's own
JavaScript debugger (CDP) on one side, and the protocol of the qjsx agent on the other
side, over the link of the device (serial port, network, or the twin).

## Start the server

In the folder of the project:

```bash
qjsx debug
```

It loads the latest package of the project in the RAM of the device and starts it
(`--no-upload`: debug what the device runs). With `--restart`, the program starts again
each time VS Code attaches, so that its startup code can be debugged; VS Code's Restart
button then restarts the program.

## Attach VS Code

`.vscode/launch.json` of the project:

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "type": "node",
            "request": "attach",
            "name": "qjsx: attach",
            "address": "127.0.0.1",
            "port": 9339,
            "autoAttachChildProcesses": false,
            "attachExistingChildren": false,
            "skipFiles": [],
            "sourceMaps": false,
            "enableContentValidation": false
        }
    ]
}
```

## What works

- breakpoints on the lines of your sources, the `debugger;` statement;
- step over, into, out, also through `await` and generators;
- pause on all exceptions or on uncaught ones only;
- the call stack, the variables of each scope, `this`, object previews;
- the debug console: expressions are compiled on the PC and run on the device;
- setting a variable;
- the output of the program in the debug console.

Conditional breakpoints, logpoints and hit counts are not there yet.

The debug information stays on the PC, next to the package: the device runs the same
bytecode with or without a debugger.
