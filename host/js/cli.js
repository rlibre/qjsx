/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Command line of qjsx (the commands not handled in C++: everything except
// 'run', 'agent' and '--version').
//
// Status messages go to stderr; stdout only carries the program output
// (monitor), so that it can be redirected or compared.
import * as fs from "sys/fs";
import { join } from "stdlib/path";
import { list as listSerialPorts } from "device/serial";
import { build } from "qjsx:build";
import { warningText } from "qjsx:compile";
import { qjsxVersion } from "sys/compiler";
import { makeLibrary } from "qjsx:library";
import { makeExecutable } from "qjsx:executable";
import { writeTypings } from "qjsx:typings";
import { loadProject, PROJECT_FILE } from "qjsx:project";
import { Device } from "qjsx:device";
import { upload, latestPackage, reducePackage } from "qjsx:deploy";
import { KIND, KEY, STREAM, kindName } from "qjsx:protocol";
import { PackageIndex } from "qjsx:debuginfo";
import { CdpServer } from "qjsx:cdp";

const HELP = `usage: qjsx <command> [arguments]

commands:
  run [options] <file.js | package.qjspkg> [arguments]
                                               run a script, or a package like the ESP does;
                                               it reaches nothing but what is granted (Deno-like):
      -A, --allow-all                     everything
      -R, --allow-read[=folder,...]       read files (everywhere without a list)
      -W, --allow-write[=folder,...]      write files
      -N, --allow-net[=host[:port],...]   network
      -E, --allow-env[=NAME,...]          environment variables
      --allow-serial[=port,...]           serial ports
      --qjsx-root-folder=<folder>         its process.cwd() (also QJSX_ROOT_FOLDER);
                                          a package adds the "desktop" section of qjsx.json
  build [directory] [--executable]             compile the project (qjsx.json) into a .qjspkg;
                                               --executable: also an executable of the PC
                                               that runs it (<output>/<name>[.exe]); also
                                               writes the typings of qjsx for VS Code
                                               (.qjsx/types, jsconfig.json when the
                                               project has none)
  mklib [directory] [--output <file>]          compile a library project into a .qjslib
  deploy [package] [options]                   send a package to a device and start it
      --target <target>   device (default: "target" of qjsx.json)
      --token <token>     token of a network agent
      --no-run            install without starting
      --ram               load it in the RAM of the device (nothing written to
                          the flash; lost at the next restart of the device)
      --reset             restart the device after installing (the program starts
                          at boot, like after a power on)
      --monitor           then show the program output (--until-exit: stop when it ends)
  monitor [options]                            show the output of the program on a device
      --target, --token, --until-exit
  device <info | run | stop | reset> [--target <target>] [--token <token>]
  debug [options]                              debugger server for VS Code (CDP, "node" attach)
      --target, --token
      --port <port>       default: "debugPort" of qjsx.json (9339)
      --host <address>    default: 127.0.0.1
      --restart           start the program again when VS Code attaches (debug its startup)
      --no-upload         debug what the device runs (default: load the latest
                          package of the project in RAM first)
  targets                                      list the serial ports
  agent [--host h] [--port p] [--storage dir] [--token t]
                                               desktop device agent (stand-in for an ESP)
      --board <name | file.json>  digital twin of a board (boards/<name>.json):
                          its native modules, its platform, its pins and parts
                          simulated
      -p, --project <dir> the twin of that project (the "twin" section of its
                          qjsx.json: board, parts, ports); without --board, the
                          project of the current folder, if it has one
      --ui-port <port>    page of the twin on http://127.0.0.1:<port> (default 8090)
  help                                         this help
  --version                                    versions of qjsx and QuickJS

targets: tcp:<host>:<port>   serial:<port>[@<baud rate>]   e.g. serial:COM3, tcp:esp.local:5555
         twin: the twin of the project on this PC (port "twin.ports.agent", 5555)`;

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

// The port of the agent of a twin when its project does not give one.
const TWIN_AGENT_PORT = 5555;

// The target of a command: --target, else "target" of qjsx.json. "twin" is
// the twin of the project on this PC ("qjsx agent" in its folder), on the
// port "twin.ports.agent" of qjsx.json, 5555 by default.
function targetOf(options, project) {
    const target = options.target ?? project?.target;
    if (target === undefined) {
        throw new Error(`no target: use --target or set "target" in ${PROJECT_FILE}`);
    }
    if (target === "twin") {
        return `tcp:127.0.0.1:${project?.twin?.ports?.agent ?? TWIN_AGENT_PORT}`;
    }
    return target;
}

function status(text) {
    console.error(`qjsx: ${text}`);
}

// A warning that must not be missed, framed with '*' (ASCII: readable in any
// console and log), its text cut into lines of 70 characters at most; 'fix'
// (optional) is a text to copy, on its own line, never cut.
const FRAME_WIDTH = 74;

function visibleWarning(text, fix) {
    // the words of the text, gathered into lines
    const lines = [];
    let line = "WARNING:";
    for (const word of text.split(/\s+/)) {
        if (line.length + 1 + word.length > FRAME_WIDTH - 4) {
            lines.push(line);
            line = word;
        } else {
            line += ` ${word}`;
        }
    }
    lines.push(line);
    if (fix !== undefined && fix !== null) {
        lines.push("");
        lines.push(`    ${fix}`);
        lines.push("");
    }

    // the frame, between blank lines
    const border = "*".repeat(FRAME_WIDTH);
    console.error("");
    status(border);
    for (const content of lines) {
        status(`* ${content}`.trimEnd());
    }
    status(border);
    console.error("");
}

// Opens the device and the session. 'monitor' gets the program output.
async function openDevice(target, token, monitor) {
    const device = await Device.open(target, monitor);
    const info = await device.hello(token);
    const firmware = device.version === undefined ? "" : `qjsx ${device.version}, `;
    status(`connected to ${info.get(KEY.AGENT)} (${firmware}QuickJS ${info.get(KEY.ENGINE)}) on ${target}`);
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

// The latest package of the project, or the one given; throws without one.
function packageToSend(positional, project) {
    const packagePath = positional[0] ?? (project && latestPackage(join(project.directory, project.output), project.name));
    if (packagePath === undefined) {
        throw new Error("no package: give its path or run 'qjsx build' first");
    }
    return packagePath;
}

// Sends a package to an open device, in flash or in RAM, and says so. A
// firmware of another qjsx version may not run it as the tool expects: said,
// not refused (the bytecode number is what must match).
async function sendPackage(device, packagePath, ram) {
    if (device.version !== undefined && device.version !== qjsxVersion) {
        status(`warning: the device runs qjsx ${device.version}, this tool is qjsx ${qjsxVersion}`);
    }

    const bytes = fs.readFileSync(packagePath);
    const buildId = await upload(device, bytes, { ram });
    const sent = `${reducePackage(bytes).length} bytes sent, build ${buildId.slice(0, 12)}`;
    if (ram) {
        status(`loaded ${packagePath} in RAM (${sent})`);
        status("warning: a package in RAM is lost at the next restart of the device; 'qjsx deploy' installs it in flash");
    } else {
        status(`installed ${packagePath} (${sent})`);
    }
}

// qjsx deploy: sends the package, then starts it (RUN), or restarts the
// device, which starts it at boot (--reset), or nothing (--no-run).
async function deployCommand(args) {
    const { options, positional } = parseArguments(args, {
        values: ["target", "token"],
        flags: ["no-run", "monitor", "until-exit", "ram", "reset"],
    });
    if (options.ram && options.reset) {
        throw new Error("--ram and --reset do not go together: a restart loses the package loaded in RAM");
    }

    const project = currentProject();
    const target = targetOf(options, project);
    const packagePath = packageToSend(positional, project);
    let monitor = monitorHandlers(true);
    let device = await openDevice(target, options.token, monitor.handlers);
    await sendPackage(device, packagePath, options.ram === true);
    if (options["no-run"]) {
        device.close();
        return 0;
    }

    // start it: RUN, or a restart of the device (the installed package starts
    // at boot, before the tool opens the session again: no STARTED event
    // then, the end of the program is what counts)
    if (options.reset) {
        device = await restartDevice(device, target, options.token, options.monitor);
        monitor = device.monitor ?? monitor;
    } else {
        await device.request(KIND.RUN);
    }

    if (!options.monitor) {
        device.close();
        return 0;
    }
    return waitForExit(device, monitor, options["until-exit"]);
}

// Sends RESET to an open device, then waits for it to come back (its link may
// go away meanwhile: native USB port) and opens the session again. With
// 'monitored', the device returned carries the monitor of its output in
// 'monitor'.
async function restartDevice(device, target, token, monitored) {
    status("restarting the device");
    // the answer may be lost with the link: the device restarts anyway; this
    // connection ends here on purpose (its handlers are not told)
    await device.request(KIND.RESET, {}, { attempts: 1 }).catch(() => {});
    device.handlers = {};
    device.close();

    const monitor = monitored ? monitorHandlers(false) : undefined;
    const again = await Device.reopen(target, monitor?.handlers ?? {}, token);
    again.monitor = monitor;
    status(`device restarted, connected again on ${target}`);
    return again;
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
        const where = answer.get(KEY.VOLATILE) === true ? ", in RAM: lost at the next restart" : "";
        console.log(`firmware: qjsx ${device.version ?? "(unknown: older firmware)"}`);
        console.log(`running:  ${answer.get(KEY.RUNNING)}`);
        console.log(`package:  ${answer.get(KEY.BUILD_ID) ?? "(none)"} (${answer.get(KEY.PACKAGE_SIZE)} bytes${where})`);
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

// qjsx build: the package of the project; with --executable, also an
// executable of the PC that runs it (<output>/<name>, .exe on Windows).
function buildCommand(args) {
    const { options, positional } = parseArguments(args, { flags: ["executable"] });
    if (positional.length > 1) {
        throw new Error("usage: qjsx build [directory] [--executable]");
    }

    // the typings for VS Code first: written even when the build fails; not
    // writing them (.qjsx is a file, a folder that cannot be written) is a
    // warning, never a failure of the build
    const directory = positional[0] ?? ".";
    const project = loadProject(directory);
    try {
        const typings = writeTypings(project.directory, project.output);
        if (typings.configWritten !== null) {
            status(`wrote ${typings.configWritten} (VS Code: the typings of qjsx, no DOM names)`);
        }
        if (typings.warning !== null) {
            visibleWarning(`VS Code will not see the typings of qjsx: ${typings.warning}`, typings.fix);
        }
    } catch (error) {
        visibleWarning(`the typings of qjsx were not written (${error.message}): VS Code will not see them.`);
    }

    // the package
    const result = build(directory);
    printWarnings(result.warnings);
    for (const note of result.notes) {
        status(note);
    }
    console.log(`built ${result.file}`);
    console.log(`  ${result.scriptCount} module(s), ${result.size} bytes, build ${result.buildId.slice(0, 12)}`);

    if (options.executable) {
        const name = process.platform === "win32" ? `${project.name}.exe` : project.name;
        const executable = makeExecutable(result.file, join(project.directory, project.output, name));
        console.log(`built ${executable.file} (${executable.size} bytes)`);
        if (process.platform !== "win32") {
            status(`mark it executable: chmod +x ${executable.file}`);
        }
    }
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
// First loads the latest package of the project in the RAM of the device
// (nothing written to the flash; --no-upload: debug what the device has) and
// starts it, or, with --restart, stops what runs: the program starts when VS
// Code attaches. Runs until Ctrl+C or until the device goes away.
async function debugCommand(args) {
    const { options, positional } = parseArguments(args, {
        values: ["target", "token", "port", "host"],
        flags: ["restart", "no-upload"],
    });
    const project = currentProject();
    if (project === undefined || positional.length > 0) {
        throw new Error(`usage: qjsx debug [--target <target>] [--port <port>] [--restart] [--no-upload], in a project folder (${PROJECT_FILE})`);
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

    // the latest package of the project, in RAM; started now, or when VS Code
    // attaches (--restart)
    if (!options["no-upload"]) {
        await sendPackage(device, packageToSend([], project), true);
        await device.request(options.restart ? KIND.STOP : KIND.RUN);
    }

    // the server
    const host = options.host ?? "127.0.0.1";
    const port = Number(options.port ?? project.debugPort);
    server = new CdpServer({
        device, index, host, port,
        title: `qjsx ${project.name}`,
        sourceCache: join(output, "debug-src"),
        restart: options.restart === true,
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

// Hidden command "twin-ui --host h --port p [--project folder] [--ui-dir folder]": the web page
// of the digital twin, started by "qjsx agent --board" on a thread of its
// own (host/agent_command.cpp); --project: the project whose parts are
// wired on the board; --ui-dir serves the page from a folder instead of the
// embedded one. Imported on demand: its native module
// "qjsx:twinboard" exists only in such an agent.
async function twinUiCommand(args) {
    const { options } = parseArguments(args, { values: ["host", "port", "project", "ui-dir"] });
    const { serveTwin } = await import("qjsx:twin/server");
    await serveTwin({ host: options.host, port: Number(options.port), projectDir: options.project, uiDir: options["ui-dir"] });
    return 0;
}

const COMMANDS = {
    "twin-ui": twinUiCommand,
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
