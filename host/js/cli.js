/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Command line of qjsx (the commands not handled in C++: everything except
// 'run', 'agent' and '--version').
//
// Status messages go to stderr; stdout only carries the program output
// (monitor), so that it can be redirected or compared.
import * as fs from "fs";
import { join } from "stdlib/path";
import { list as listSerialPorts } from "serial";
import { build } from "qjsx:build";
import { warningText } from "qjsx:compile";
import { makeLibrary } from "qjsx:library";
import { loadProject, PROJECT_FILE } from "qjsx:project";
import { Device } from "qjsx:device";
import { upload, latestPackage } from "qjsx:deploy";
import { KIND, KEY, STREAM, kindName } from "qjsx:protocol";
import { PackageIndex } from "qjsx:debuginfo";
import { CdpServer } from "qjsx:cdp";

const HELP = `usage: qjsx <command> [arguments]

commands:
  run <file.js | package.qjspkg> [arguments]   run a script, or a package like the ESP does
  build [directory]                            compile the project (qjsx.json) into a .qjspkg
  mklib [directory] [--output <file>]          compile a library project into a .qjslib
  deploy [package] [options]                   send a package to a device and start it
      --target <target>   device (default: "target" of qjsx.json)
      --token <token>     token of a network agent
      --no-run            install without starting
      --monitor           then show the program output (--until-exit: stop when it ends)
  monitor [options]                            show the output of the program on a device
      --target, --token, --until-exit
  device <info | run | stop | reset> [--target <target>] [--token <token>]
  debug [options]                              debugger server for VS Code (CDP, "node" attach)
      --target, --token
      --port <port>       default: "debugPort" of qjsx.json (9339)
      --host <address>    default: 127.0.0.1
  targets                                      list the serial ports
  agent [--host h] [--port p] [--storage dir] [--token t]
                                               desktop device agent (stand-in for an ESP)
  help                                         this help
  --version                                    versions of qjsx and QuickJS

targets: tcp:<host>:<port>   serial:<port>[@<baud rate>]   e.g. serial:COM3, tcp:esp.local:5555`;

// '--name value' options and '--flag' flags; everything else is positional.
function parseArguments(args, { values = [], flags = [] }) {
    const options = {};
    const positional = [];
    for (let i = 0; i < args.length; i++) {
        const arg = args[i];
        if (!arg.startsWith("--")) {
            positional.push(arg);
            continue;
        }
        const name = arg.slice(2);
        if (values.includes(name)) {
            if (i + 1 >= args.length) {
                throw new Error(`${arg} needs a value`);
            }
            options[name] = args[++i];
        } else if (flags.includes(name)) {
            options[name] = true;
        } else {
            throw new Error(`unknown option ${arg}`);
        }
    }
    return { options, positional };
}

function currentProject() {
    return fs.existsSync(PROJECT_FILE) ? loadProject(".") : undefined;
}

function targetOf(options, project) {
    const target = options.target ?? project?.target;
    if (target === undefined) {
        throw new Error(`no target: use --target or set "target" in ${PROJECT_FILE}`);
    }
    return target;
}

function status(text) {
    console.error(`qjsx: ${text}`);
}

// Opens the device and the session. 'monitor' gets the program output.
async function openDevice(target, token, monitor) {
    const device = await Device.open(target, monitor);
    const info = await device.hello(token);
    status(`connected to ${info.get(KEY.AGENT)} (QuickJS ${info.get(KEY.ENGINE)}) on ${target}`);
    return device;
}

// Program output to stdout / stderr, and the end of the program. With
// 'afterStart', only an end that follows a start counts: a deployment first
// stops the program already running, whose end must not end the wait.
function monitorHandlers(afterStart = false) {
    let started = !afterStart;
    let onExit = () => {};
    const exited = new Promise(resolve => { onExit = resolve; });
    const decoder = new TextDecoder();
    const handlers = {
        onLog(stream, text) {
            if (stream === STREAM.STDERR) {
                process.stderr.write(text);
            } else {
                process.stdout.write(text);
            }
        },
        onText(bytes) {
            process.stdout.write(decoder.decode(bytes));
        },
        onEvent(kind, payload) {
            if (kind === KIND.EXITED) {
                status(`program ended (code ${payload.get(KEY.CODE)})`);
                if (started) {
                    onExit(payload.get(KEY.CODE));
                }
            } else if (kind === KIND.STARTED) {
                started = true;
                status(`program started (build ${String(payload.get(KEY.BUILD_ID)).slice(0, 12)})`);
            }
        },
        onClose() {
            status("connection closed");
            onExit(1);
        },
    };
    return { handlers, exited };
}

async function deployCommand(args) {
    const { options, positional } = parseArguments(args, {
        values: ["target", "token"],
        flags: ["no-run", "monitor", "until-exit"],
    });
    const project = currentProject();
    const target = targetOf(options, project);
    const packagePath = positional[0] ?? (project && latestPackage(join(project.directory, project.output), project.name));
    if (packagePath === undefined) {
        throw new Error("no package: give its path or run 'qjsx build' first");
    }
    const bytes = fs.readFileSync(packagePath);
    const monitor = monitorHandlers(true);
    const device = await openDevice(target, options.token, monitor.handlers);
    const buildId = await upload(device, bytes);
    status(`installed ${packagePath} (${bytes.length} bytes, build ${buildId.slice(0, 12)})`);
    if (options["no-run"]) {
        device.close();
        return 0;
    }
    await device.request(KIND.RUN);
    if (!options.monitor) {
        device.close();
        return 0;
    }
    return waitForExit(device, monitor, options["until-exit"]);
}

async function waitForExit(device, monitor, untilExit) {
    if (!untilExit) {
        await new Promise(() => {});   // until Ctrl+C or the connection closes
    }
    const code = await monitor.exited;
    device.close();
    return code;
}

async function monitorCommand(args) {
    const { options } = parseArguments(args, { values: ["target", "token"], flags: ["until-exit"] });
    const monitor = monitorHandlers();
    const device = await openDevice(targetOf(options, currentProject()), options.token, monitor.handlers);
    return waitForExit(device, monitor, options["until-exit"]);
}

async function deviceCommand(args) {
    const { options, positional } = parseArguments(args, { values: ["target", "token"] });
    const action = positional[0];
    const requests = { info: KIND.INFO, run: KIND.RUN, stop: KIND.STOP, reset: KIND.RESET };
    if (requests[action] === undefined || positional.length !== 1) {
        throw new Error("usage: qjsx device <info | run | stop | reset> [--target <target>]");
    }
    const device = await openDevice(targetOf(options, currentProject()), options.token, {});
    const answer = await device.request(requests[action]);
    if (action === "info") {
        console.log(`running:  ${answer.get(KEY.RUNNING)}`);
        console.log(`package:  ${answer.get(KEY.BUILD_ID) ?? "(none)"} (${answer.get(KEY.PACKAGE_SIZE)} bytes)`);
    } else {
        status(`${kindName(requests[action]).toLowerCase()} done`);
    }
    device.close();
    return 0;
}

function targetsCommand() {
    const ports = listSerialPorts();
    if (ports.length === 0) {
        console.log("no serial port found");
    }
    for (const port of ports) {
        console.log(`serial:${port}`);
    }
    return 0;
}

// Compile warnings on stderr, like a compiler: "file:line:column: warning: text".
// They do not make the command fail.
function printWarnings(warnings) {
    for (const warning of warnings) {
        console.error(`${warning.file}:${warning.line}:${warning.column}: warning: ${warningText(warning)}`);
    }
}

function buildCommand(args) {
    if (args.length > 1) {
        throw new Error("usage: qjsx build [directory]");
    }
    const result = build(args[0] ?? ".");
    printWarnings(result.warnings);
    for (const note of result.notes) {
        status(note);
    }
    console.log(`built ${result.file}`);
    console.log(`  ${result.scriptCount} module(s), ${result.size} bytes, build ${result.buildId.slice(0, 12)}`);
    return 0;
}

function mklibCommand(args) {
    const { options, positional } = parseArguments(args, { values: ["output"] });
    if (positional.length > 1) {
        throw new Error("usage: qjsx mklib [directory] [--output <file>]");
    }
    const result = makeLibrary(positional[0] ?? ".", options.output);
    printWarnings(result.warnings);
    console.log(`built ${result.file}`);
    console.log(`  library ${result.name} ${result.version}, ${result.moduleCount} module(s), ${result.size} bytes`);
    return 0;
}

// qjsx debug: the debugger server (CDP) of the project's device, for VS Code.
// Runs until Ctrl+C or until the device goes away.
async function debugCommand(args) {
    const { options, positional } = parseArguments(args, { values: ["target", "token", "port", "host"] });
    const project = currentProject();
    if (project === undefined || positional.length > 0) {
        throw new Error(`usage: qjsx debug [--target <target>] [--port <port>], in a project folder (${PROJECT_FILE})`);
    }

    // the packages of the project, to find the one the device runs
    const output = join(project.directory, project.output);
    const index = new PackageIndex([output], { sourceRoot: project.directory });

    // the device; its program output goes through the server
    let server = null;
    let deviceClosed;
    const closed = new Promise(resolve => {
        deviceClosed = resolve;
    });
    const handlers = {
        onLog: (stream, text) => server?.output(stream, text),
        onClose: () => deviceClosed(),
    };
    const device = await openDevice(targetOf(options, project), options.token, handlers);

    // the server
    const host = options.host ?? "127.0.0.1";
    const port = Number(options.port ?? project.debugPort);
    server = new CdpServer({
        device, index, host, port,
        title: `qjsx ${project.name}`,
        sourceCache: join(output, "debug-src"),
        onStatus: status,
    });
    server.start();
    status(`debugger server on ${host}:${server.port()} (VS Code: "node" attach to this port)`);

    // until the device goes away (or Ctrl+C)
    await closed;
    status("device connection closed");
    await server.close();
    return 1;
}

const COMMANDS = {
    debug: debugCommand,
    build: buildCommand,
    mklib: mklibCommand,
    deploy: deployCommand,
    monitor: monitorCommand,
    device: deviceCommand,
    targets: targetsCommand,
};

export async function main(args) {
    const [command, ...rest] = args;
    if (command === undefined || command === "help" || command === "--help") {
        console.log(HELP);
        return;
    }
    try {
        const run = COMMANDS[command];
        if (run === undefined) {
            throw new Error(`unknown command '${command}' (see 'qjsx help')`);
        }
        const code = await run(rest);
        process.exit(code ?? 0);
    } catch (error) {
        console.error(`qjsx: ${error.message}`);
        // A syntax error of a compiled module: the stack gives file:line:column.
        if (error instanceof SyntaxError && error.stack) {
            console.error(error.stack.trimEnd());
        }
        process.exit(1);
    }
}
