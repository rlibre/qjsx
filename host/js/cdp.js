/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// CDP server of the debugger (qjsx debug): VS Code's js-debug attaches to it
// as to a Node.js inspector ("type": "node", "request": "attach") and debugs
// the program of a device through a DebugSession (qjsx:debugsession).
//
//   const server = new CdpServer({ device, index, port: 9339, sourceCache });
//   server.start();
//   // device output: server.output(stream, text)
//
// What js-debug does (read in its sources, see doc/architecture.md 3.9):
// GET /json/version and /json/list, then a WebSocket; an error answer to a
// command is almost never fatal (the commands we do not serve get "not
// found"); scripts are file:/// URLs without hash; breakpoints come as
// Debugger.setBreakpointByUrl with a urlRegex; destroying the default
// execution context would close its session, so it is never destroyed (the
// session survives the end of the program and picks up the next run).
//
// One client at a time: a new one replaces the previous one. The device is
// attached (DEBUG_ATTACH) when the client first needs it and detached when
// it leaves (stops removed, a paused program resumes).
//
// Identifiers given to js-debug:
//   scriptId      a number per module and per run ("7")
//   callFrameId   "<pause number>:<level of the device frame>"
//   objectId      "h:<handle>" for an object of the device,
//                 "scope:<pause number>:<level>:<index>" for a scope
//   breakpointId  "bp:<number>"
// A callFrameId or a scope of a pause that is over is refused.
import * as fs from "sys/fs";
import { dirname, resolve } from "stdlib/path";
import { qjsxVersion } from "sys/compiler";
import { startServer } from "qjsx:websocket";
import { DebugSession } from "qjsx:debugsession";
import { KIND } from "qjsx:protocol";

export const DEFAULT_PORT = 9339;
const TARGET_ID = "qjsx";
const CONTEXT_ID = 1;

// JSON-RPC error codes of CDP.
const METHOD_NOT_FOUND = -32601;
const SERVER_ERROR = -32000;

// Object previews (Runtime.getProperties and evaluations with
// generatePreview): at most this many properties, values cut to this length.
const PREVIEW_PROPERTIES = 5;
const PREVIEW_TEXT = 100;

// Methods accepted and ignored: js-debug sends them, nothing here needs them.
const IGNORED_METHODS = new Set([
    "Profiler.enable", "Profiler.disable", "NodeWorker.enable",
    "NodeWorker.disable", "Debugger.setAsyncCallStackDepth", "Runtime.setAsyncCallStackDepth",
    "Debugger.setBlackboxPatterns", "Debugger.setBreakpointsActive", "Runtime.releaseObjectGroup",
    "Runtime.disable", "Debugger.disable",
]);

// CDP subtype of a kind of object of the device (decodeValue of qjsx:debugsession).
const SUBTYPES = {
    array: "array", error: "error", date: "date", regexp: "regexp", map: "map", set: "set",
    weakmap: "weakmap", weakset: "weakset", promise: "promise", proxy: "proxy",
    typed_array: "typedarray", array_buffer: "arraybuffer", dataview: "dataview",
    generator: "generator", iterator: "iterator", weakref: "weakref",
};

/**
 * An error answered to a command, with its JSON-RPC code.
 */
class CdpError extends Error {
    /**
     * 'code': a JSON-RPC error code (SERVER_ERROR by default).
     */
    constructor(message, code = SERVER_ERROR) {
        super(message);
        this.code = code;
    }
}

/**
 * The CDP server of a device (see the top of the file).
 */
export class CdpServer {
    /**
     * device: an opened Device (hello done); index: a PackageIndex;
     * host, port: where to listen; title: shown by the debugger;
     * sourceCache: folder where the sources of the library modules are
     * written (js-debug opens files); restart: start the program again when
     * a client is ready (Runtime.runIfWaitingForDebugger, sent by js-debug
     * once its breakpoints are set), so that startup code can be debugged;
     * onStatus(text): messages for the user
     */
    constructor({ device, index, host = "127.0.0.1", port = DEFAULT_PORT, title = "qjsx", sourceCache, restart = false, onStatus }) {
        this.device = device;
        this.index = index;
        this.host = host;
        this.listenPort = port;
        this.title = title;
        this.sourceCache = sourceCache;
        this.restart = restart;
        this.onStatus = onStatus ?? (() => {});

        this.server = null;
        this.client = null;            // WebSocket of the debugger; null: none
        this.resetClientState();
    }

    /**
     * What belongs to one client: forgotten when it leaves.
     */
    resetClientState() {
        this.session = null;            // DebugSession, made when the client needs the device
        this.sessionReady = null;       // promise of its attach()
        this.runtimeEnabled = false;
        this.debuggerEnabled = false;
        this.runNumber = 0;
        this.nextScriptId = 1;
        this.scriptIds = new Map();     // module -> scriptId (this run)
        this.scriptsById = new Map();   // scriptId -> { module, script, url }
        this.urls = new Map();          // module -> file:/// URL (this package)
        this.nextBreakpointId = 1;
        this.breakpoints = new Map();   // breakpointId -> { id, url, urlRegex, line, column, bindings: Map(module -> session id) }
        this.pauseNumber = 0;
        this.pause = null;              // { number, frames, scopes: Map(level -> scopes) } while paused
        this.lineBuffers = { 1: "", 2: "" };
    }

    // ### Server ##############################################################

    /**
     * Starts listening; throws if the port is taken.
     */
    start() {
        this.server = startServer({
            host: this.host,
            port: this.listenPort,
            routes: path => this.route(path),
            acceptsSocket: path => path === `/${TARGET_ID}`,
            onSocket: socket => this.onSocket(socket),
        });
    }

    /**
     * The port the server listens on (the one chosen when 0 was asked).
     */
    port() {
        return this.server.port();
    }

    /**
     * The URL of the WebSocket of the debugger.
     */
    webSocketUrl() {
        return `ws://${this.host}:${this.port()}/${TARGET_ID}`;
    }

    /**
     * The HTTP answers of the inspector protocol.
     */
    route(path) {
        // the version
        if (path === "/json/version") {
            return { Browser: `qjsx/${qjsxVersion}`, "Protocol-Version": "1.3" };
        }

        // the list of the targets: one, the device
        if (path === "/json/list" || path === "/json") {
            return [{
                description: "qjsx device",
                id: TARGET_ID,
                title: this.title,
                type: "node",
                url: "file://",
                webSocketDebuggerUrl: this.webSocketUrl(),
            }];
        }

        return undefined;
    }

    /**
     * A debugger connects: it replaces the previous one.
     */
    onSocket(socket) {
        // the previous client, if any, is ended
        if (this.client !== null) {
            const previous = this.client;
            this.endClient(previous);
            previous.close();
        }

        // the new one
        this.client = socket;
        socket.onMessage(text => this.onClientMessage(socket, text));
        socket.onClose(() => this.endClient(socket));
        this.onStatus("debugger connected");
    }

    /**
     * The client left (or was replaced): the device goes on without debugger.
     */
    endClient(socket) {
        // a client replaced already
        if (this.client !== socket) {
            return;
        }

        // forget it, then detach the device (in the background)
        const session = this.session;
        this.client = null;
        this.resetClientState();
        this.onStatus("debugger disconnected");
        session?.detach().catch(error => this.onStatus(`detach: ${error.message}`));
    }

    /**
     * Closes the client, then stops listening.
     */
    async close() {
        if (this.client !== null) {
            this.client.close();
        }

        this.server?.close();
    }

    // ### Messages ############################################################

    /**
     * One command of the client: its answer, or an error.
     */
    async onClientMessage(socket, text) {
        // a JSON message; anything else is ignored
        let message;
        try {
            message = JSON.parse(text);
        } catch (error) {
            return;
        }

        // the command, answered with its result or its error
        const { id, method, params } = message;
        try {
            const result = await this.handle(method, params ?? {});
            this.sendTo(socket, { id, result: result ?? {} });
        } catch (error) {
            const code = error instanceof CdpError ? error.code : SERVER_ERROR;
            this.sendTo(socket, { id, error: { code, message: error.message } });
        }
    }

    /**
     * A message to a client, unless it was replaced meanwhile.
     */
    sendTo(socket, message) {
        if (socket === this.client) {
            socket.send(JSON.stringify(message));
        }
    }

    /**
     * An event to the client, if there is one.
     */
    event(method, params) {
        if (this.client !== null) {
            this.client.send(JSON.stringify({ method, params }));
        }
    }

    // ### Program output ######################################################

    /**
     * Output of the program of the device: to the debug console of the
     * client (one message per line), else to this terminal.
     */
    output(stream, text) {
        // no debug console: to this terminal
        const hasNoConsole = this.client === null || !this.runtimeEnabled;
        if (hasNoConsole) {
            const out = stream === 2 ? process.stderr : process.stdout;
            out.write(text);
            return;
        }

        // whole lines only; the rest waits for its end
        const buffered = this.lineBuffers[stream] + text;
        const lines = buffered.split("\n");
        this.lineBuffers[stream] = lines.pop();

        for (const line of lines) {
            this.consoleMessage(stream === 2 ? "error" : "log", line);
        }
    }

    /**
     * A line in the debug console of the client ('type': "log", "error", "info").
     */
    consoleMessage(type, text) {
        this.event("Runtime.consoleAPICalled", {
            type,
            args: [{ type: "string", value: text }],
            executionContextId: CONTEXT_ID,
            timestamp: Date.now(),
        });
    }

    // ### Session #############################################################

    /**
     * The debugging session of the client, attached on first need.
     */
    async ensureSession() {
        // made and attached once
        if (this.sessionReady === null) {
            this.session = new DebugSession(this.device, this.index, {
                onPaused: pause => this.onPaused(pause),
                onResumed: () => this.onResumed(),
                onScriptLoaded: module => this.announceScript(module),
                onStarted: () => this.onStarted(),
                onExited: code => this.onExited(code),
                onError: error => this.onStatus(`debugger: ${error.message}`),
            });

            this.sessionReady = this.session.attach();
        }

        await this.sessionReady;
        return this.session;
    }

    /**
     * A new run: new scripts, the breakpoints bound again in its package.
     */
    onStarted() {
        // nothing announced in this run yet
        this.runNumber++;
        this.scriptIds.clear();
        this.scriptsById.clear();
        this.urls.clear();
        this.pause = null;

        // the context of the run
        if (this.runtimeEnabled) {
            this.event("Runtime.executionContextsCleared", {});
            this.announceContext();
        }

        // the breakpoints of the client in the package of the run (in the background)
        for (const breakpoint of this.breakpoints.values()) {
            this.bindBreakpoint(breakpoint).catch(error => this.onStatus(`breakpoint: ${error.message}`));
        }
    }

    /**
     * The program ended: told in the debug console and to the user.
     */
    onExited(code) {
        this.pause = null;
        if (this.runtimeEnabled) {
            this.consoleMessage("info", `qjsx: program ended (code ${code})`);
        }

        this.onStatus(`program ended (code ${code})`);
    }

    /**
     * Runtime.executionContextCreated: the default context of the run.
     */
    announceContext() {
        this.event("Runtime.executionContextCreated", {
            context: { id: CONTEXT_ID, origin: "", name: this.title, uniqueId: `qjsx-${this.runNumber}`, auxData: { isDefault: true } },
        });
    }

    // ### Scripts #############################################################

    /**
     * file:/// URL of a script: its file in the project, or for a library
     * module a copy of its source in the cache folder (js-debug opens files).
     */
    urlOf(script) {
        // made once per package
        const known = this.urls.get(script.module);
        if (known !== undefined) {
            return known;
        }

        // the file of the project, or the copy of a library module
        let path;
        if (script.localPath !== null) {
            path = resolve(script.localPath);
        } else {
            path = resolve(this.sourceCache, `${script.module}.js`);
            writeIfChanged(path, script.source);
        }

        const url = fileUrl(path);
        this.urls.set(script.module, url);
        return url;
    }

    /**
     * Debugger.scriptParsed for a loaded module (once per run), then the
     * breakpoints that resolve in it.
     */
    announceScript(module) {
        // only once, and only a module the package knows
        const script = this.session?.package?.script(module);
        const cannotAnnounce = !this.debuggerEnabled || script === undefined || this.scriptIds.has(module);
        if (cannotAnnounce) {
            return;
        }

        // its id and its URL
        const scriptId = String(this.nextScriptId++);
        const url = this.urlOf(script);
        this.scriptIds.set(module, scriptId);
        this.scriptsById.set(scriptId, { module, script, url });

        // the script
        const lineCount = script.lines.byteStarts.length;
        this.event("Debugger.scriptParsed", {
            scriptId,
            url,
            startLine: 0,
            startColumn: 0,
            endLine: lineCount - 1,
            endColumn: 0,
            executionContextId: CONTEXT_ID,
            hash: "",
            isModule: true,
            length: script.source.length,
            scriptLanguage: "JavaScript",
            embedderName: url,
            hasSourceURL: false,
        });

        // the breakpoints bound in it
        for (const breakpoint of this.breakpoints.values()) {
            const location = this.locationOf(breakpoint, module);
            if (location !== null) {
                this.event("Debugger.breakpointResolved", { breakpointId: breakpoint.id, location });
            }
        }
    }

    /**
     * The script of a scriptId given by the client.
     */
    scriptOf(scriptId) {
        const entry = this.scriptsById.get(scriptId);
        if (entry === undefined) {
            throw new CdpError(`no script ${scriptId}`);
        }

        return entry;
    }

    // ### Breakpoints #########################################################

    /**
     * Sets a breakpoint of the session in every module of the package whose
     * URL the CDP breakpoint matches and that has none yet.
     */
    async bindBreakpoint(breakpoint) {
        const pkg = this.session?.package;
        if (pkg === undefined) {
            return;
        }

        for (const script of pkg.scripts()) {
            // bound already, or another URL
            const isSkipped = breakpoint.bindings.has(script.module) || !matchesUrl(breakpoint, this.urlOf(script));
            if (isSkipped) {
                continue;
            }

            // a breakpoint of the session, if the module has a statement from there
            const set = await this.session.setBreakpoint(script.module, breakpoint.line, breakpoint.column);
            if (set !== null) {
                breakpoint.bindings.set(script.module, set.id);
            }
        }
    }

    /**
     * CDP location of a breakpoint in a module announced to the client, or null.
     */
    locationOf(breakpoint, module) {
        // bound in the module, resolved, and the module announced
        const sessionId = breakpoint.bindings.get(module);
        const scriptId = this.scriptIds.get(module);
        const found = sessionId === undefined ? undefined : this.session.breakpointLocation(sessionId);
        const isUnknown = scriptId === undefined || found?.location === null || found === undefined;
        if (isUnknown) {
            return null;
        }

        return { scriptId, lineNumber: found.location.line, columnNumber: found.location.column };
    }

    /**
     * The CDP locations of a breakpoint in every announced module.
     */
    locationsOf(breakpoint) {
        return [...breakpoint.bindings.keys()]
            .map(module => this.locationOf(breakpoint, module))
            .filter(location => location !== null);
    }

    /**
     * A breakpoint of the client: refused with a condition (conditional
     * breakpoints and logpoints come with step 4), bound to the modules it
     * matches.
     */
    async addBreakpoint({ url, urlRegex, line, column, condition }) {
        const hasCondition = condition !== undefined && condition !== "";
        if (hasCondition) {
            throw new CdpError("conditional breakpoints and logpoints are not supported yet");
        }

        // kept, then bound
        await this.ensureSession();
        const breakpoint = { id: `bp:${this.nextBreakpointId++}`, url, urlRegex, line, column, bindings: new Map() };
        this.breakpoints.set(breakpoint.id, breakpoint);
        await this.bindBreakpoint(breakpoint);
        return breakpoint;
    }

    /**
     * The breakpoints of the client hit at the pause: those bound at the
     * line of the top frame.
     */
    hitBreakpoints(frame) {
        const hits = [];

        for (const breakpoint of this.breakpoints.values()) {
            const sessionId = breakpoint.bindings.get(frame.module);
            const found = sessionId === undefined ? undefined : this.session.breakpointLocation(sessionId);
            if (found?.location?.line === frame.line) {
                hits.push(breakpoint.id);
            }
        }

        return hits;
    }

    // ### Pauses ##############################################################

    /**
     * The program paused: Debugger.paused with the frames, once their
     * scripts are announced.
     */
    async onPaused(pause) {
        // a new pause number: the ids of the previous pause become invalid
        this.pauseNumber++;
        const number = this.pauseNumber;
        this.pause = { number, frames: pause.frames, scopes: new Map() };

        // the scripts of the stack are announced before the pause
        for (const frame of pause.frames) {
            if (!frame.native) {
                this.announceScript(frame.module);
            }
        }

        // the frames, with their 'this' read on the device (one request per
        // frame whose function uses 'this')
        const shown = pause.frames.filter(frame => !frame.native && this.scriptIds.has(frame.module) && frame.line !== undefined);
        const callFrames = [];

        for (const frame of shown) {
            const thisValue = await this.session.thisOf(frame.level);
            callFrames.push(this.callFrame(frame, thisValue));
        }

        // the program may have gone on meanwhile (the client left, a stop)
        const isOver = this.pause === null || this.pause.number !== number;
        if (isOver) {
            return;
        }

        // the return value, shown in the top frame
        const hasReturnValue = callFrames.length > 0 && pause.returnValue !== undefined;
        if (hasReturnValue) {
            callFrames[0].returnValue = remoteObject(pause.returnValue);
        }

        // the reason, with the exception or the breakpoints hit
        const params = { callFrames, reason: pause.reason === "exception" ? "exception" : "other" };
        if (pause.exception !== undefined) {
            params.data = { ...remoteObject(pause.exception), uncaught: pause.uncaught === true };
        }

        const isBreakpointHit = pause.reason === "breakpoint" && pause.frames.length > 0;
        if (isBreakpointHit) {
            params.hitBreakpoints = this.hitBreakpoints(pause.frames[0]);
        }

        this.event("Debugger.paused", params);
    }

    /**
     * The program resumed: Debugger.resumed.
     */
    onResumed() {
        this.pause = null;
        this.event("Debugger.resumed", {});
    }

    /**
     * A frame of the device as a CDP call frame; the scopes are objects whose
     * properties are read on demand (Runtime.getProperties). 'thisValue':
     * undefined when the function has no 'this' slot.
     */
    callFrame(frame, thisValue) {
        // the scopes, as objects known by an id
        const number = this.pause.number;
        const scopeChain = this.session.scopesOf(frame).map((scope, index) => ({
            type: scope.kind,
            object: {
                type: "object",
                className: "Object",
                description: scope.kind,
                objectId: `scope:${number}:${frame.level}:${index}`,
            },
        }));

        // the frame
        return {
            callFrameId: `${number}:${frame.level}`,
            functionName: frame.functionName,
            location: { scriptId: this.scriptIds.get(frame.module), lineNumber: frame.line, columnNumber: frame.column },
            url: this.urls.get(frame.module),
            scopeChain,
            this: thisValue === undefined ? { type: "undefined" } : remoteObject(thisValue),
            canBeRestarted: false,
        };
    }

    /**
     * The device level of a callFrameId of the current pause.
     */
    levelOf(callFrameId) {
        const [number, level] = String(callFrameId).split(":").map(Number);
        const isOver = this.pause === null || number !== this.pause.number;
        if (isOver) {
            throw new CdpError("this call frame belongs to a pause that is over");
        }

        return level;
    }

    /**
     * The variables of a scope object ("scope:<pause>:<level>:<index>").
     */
    async scopeVariables(objectId) {
        // a scope of the current pause
        const [, number, level, index] = objectId.split(":").map(Number);
        const isOver = this.pause === null || number !== this.pause.number;
        if (isOver) {
            throw new CdpError("this scope belongs to a pause that is over");
        }

        // the scopes of the frame, read once per pause
        let scopes = this.pause.scopes.get(level);
        if (scopes === undefined) {
            scopes = await this.session.scopes(level);
            this.pause.scopes.set(level, scopes);
        }

        return scopes[index]?.variables ?? [];
    }

    // ### Previews ############################################################

    /**
     * An evaluation result with the preview of its value, when asked.
     */
    async withPreview(result, generatePreview) {
        await this.addPreviews([result.result], generatePreview);
        return result;
    }

    /**
     * Adds a preview to each object of 'remotes' (RemoteObjects), when
     * asked: one request to the device per object, sent together.
     */
    async addPreviews(remotes, generatePreview) {
        if (generatePreview !== true) {
            return;
        }

        // the objects of the device only (not the scopes)
        const objects = remotes.filter(remote => remote.type === "object" && remote.objectId?.startsWith("h:"));
        await Promise.all(objects.map(async remote => {
            try {
                remote.preview = await this.previewOf(remote);
            } catch (error) {
                // no preview: the description is enough
            }
        }));
    }

    /**
     * The CDP ObjectPreview of an object of the device: its first enumerable
     * own properties, each value as a short text (an object by its
     * description); getters are never called.
     */
    async previewOf(remote) {
        // the enumerable properties named by a text
        const { properties } = await this.session.properties(handleOf(remote.objectId));
        const shown = properties.filter(property => property.enumerable && typeof property.name === "string");

        // the first ones, an accessor without its value
        const entries = shown.slice(0, PREVIEW_PROPERTIES).map(property => {
            if (property.accessor) {
                return { name: property.name, type: "accessor" };
            }

            const value = remoteObject(property.value);
            const entry = { name: property.name, type: value.type, value: previewText(value) };
            if (value.subtype !== undefined) {
                entry.subtype = value.subtype;
            }

            return entry;
        });

        // the preview
        const preview = {
            type: remote.type,
            description: remote.description,
            overflow: shown.length > PREVIEW_PROPERTIES,
            properties: entries,
        };

        if (remote.subtype !== undefined) {
            preview.subtype = remote.subtype;
        }

        return preview;
    }

    // ### Commands ############################################################

    /**
     * One CDP command: its result.
     */
    async handle(method, params) {
        if (IGNORED_METHODS.has(method)) {
            return {};
        }

        // the handler of the command, called on this server
        const handler = COMMANDS[method];
        if (handler === undefined) {
            throw new CdpError(`'${method}' wasn't found`, METHOD_NOT_FOUND);
        }

        return handler.call(this, params);
    }
}

// ### Commands ################################################################

// The commands served, by CDP method; each one is called with the server as
// 'this' and the params of the command.
const COMMANDS = {
    /**
     * The program output goes to the debug console from now on.
     */
    "Runtime.enable"() {
        this.runtimeEnabled = true;
        // the context once the answer is gone: after the current task
        Promise.resolve().then(() => this.announceContext());
        return {};
    },

    /**
     * Attaches the device, then announces the modules loaded already.
     */
    async "Debugger.enable"() {
        this.debuggerEnabled = true;
        const session = await this.ensureSession();

        for (const module of session.loadedModules) {
            this.announceScript(module);
        }

        return { debuggerId: "qjsx" };
    },

    /**
     * A breakpoint by URL (or URL regular expression), in every module that matches.
     */
    async "Debugger.setBreakpointByUrl"({ url, urlRegex, lineNumber, columnNumber, condition }) {
        const breakpoint = await this.addBreakpoint({ url, urlRegex, line: lineNumber, column: columnNumber ?? 0, condition });
        return { breakpointId: breakpoint.id, locations: this.locationsOf(breakpoint) };
    },

    /**
     * A breakpoint in an announced script: refused where no statement follows.
     */
    async "Debugger.setBreakpoint"({ location, condition }) {
        const { url } = this.scriptOf(location.scriptId);
        const breakpoint = await this.addBreakpoint({ url, line: location.lineNumber, column: location.columnNumber ?? 0, condition });
        const locations = this.locationsOf(breakpoint);
        if (locations.length === 0) {
            throw new CdpError("no statement there");
        }

        return { breakpointId: breakpoint.id, actualLocation: locations[0] };
    },

    /**
     * Removes a breakpoint of the client and its breakpoints of the session.
     */
    async "Debugger.removeBreakpoint"({ breakpointId }) {
        const breakpoint = this.breakpoints.get(breakpointId);
        if (breakpoint === undefined) {
            return {};
        }

        this.breakpoints.delete(breakpointId);

        for (const sessionId of breakpoint.bindings.values()) {
            await this.session.removeBreakpoint(sessionId);
        }

        return {};
    },

    /**
     * Where breakpoints can be in a range of a script.
     */
    "Debugger.getPossibleBreakpoints"({ start, end }) {
        const { script } = this.scriptOf(start.scriptId);
        const found = script.breakableLocations(start.lineNumber, start.columnNumber ?? 0, end?.lineNumber, end?.columnNumber ?? 0);
        return {
            locations: found.map(({ line, column }) => ({ scriptId: start.scriptId, lineNumber: line, columnNumber: column, type: "break" })),
        };
    },

    /**
     * The source of a script, as in the package.
     */
    "Debugger.getScriptSource"({ scriptId }) {
        return { scriptSource: this.scriptOf(scriptId).script.source };
    },

    /**
     * Pause on exceptions: "none", "uncaught", or "all" (CDP's "caught" too).
     */
    async "Debugger.setPauseOnExceptions"({ state }) {
        const session = await this.ensureSession();
        await session.setPauseOnExceptions(state === "caught" ? "all" : state);
        return {};
    },

    /**
     * Pauses the program.
     */
    async "Debugger.pause"() {
        await (await this.ensureSession()).pause();
        return {};
    },

    /**
     * Resumes the program.
     */
    async "Debugger.resume"() {
        await (await this.ensureSession()).resume();
        return {};
    },

    /**
     * Steps over.
     */
    async "Debugger.stepOver"() {
        await (await this.ensureSession()).stepOver();
        return {};
    },

    /**
     * Steps into.
     */
    async "Debugger.stepInto"() {
        await (await this.ensureSession()).stepInto();
        return {};
    },

    /**
     * Steps out.
     */
    async "Debugger.stepOut"() {
        await (await this.ensureSession()).stepOut();
        return {};
    },

    /**
     * With --restart: the client has set its breakpoints, the program starts
     * again under the debugger (each module waits for its stops when it is
     * loaded). Otherwise nothing to do: the program already runs.
     */
    async "Runtime.runIfWaitingForDebugger"() {
        if (!this.restart) {
            return {};
        }

        await this.ensureSession();
        await this.device.request(KIND.RUN);
        this.onStatus("program started again for the debugger (--restart)");
        return {};
    },

    /**
     * Evaluates an expression in a frame of the pause.
     */
    async "Debugger.evaluateOnCallFrame"({ callFrameId, expression, returnByValue, generatePreview }) {
        const level = this.levelOf(callFrameId);
        const answer = await this.session.evaluate(expression, { level, returnByValue: returnByValue === true });
        return this.withPreview(evaluationResult(answer), generatePreview);
    },

    /**
     * Evaluates an expression in the top frame when paused, else in the global scope.
     */
    async "Runtime.evaluate"({ expression, returnByValue, generatePreview }) {
        const session = await this.ensureSession();
        const answer = await session.evaluate(expression, { level: 0, returnByValue: returnByValue === true });
        return this.withPreview(evaluationResult(answer), generatePreview);
    },

    /**
     * Calls a function given by its source on an object of the device (a
     * scope object gives no 'this').
     */
    async "Runtime.callFunctionOn"({ functionDeclaration, objectId, arguments: callArguments, returnByValue, generatePreview }) {
        const session = await this.ensureSession();
        const hasNoThis = objectId === undefined || objectId.startsWith("scope:");
        const thisValue = hasNoThis ? undefined : valueOfObjectId(objectId);
        const args = (callArguments ?? []).map(fromCallArgument);
        const answer = await session.callFunction(functionDeclaration, { thisValue, args, returnByValue: returnByValue === true });
        return this.withPreview(evaluationResult(answer), generatePreview);
    },

    /**
     * The properties of an object of the device, or the variables of a scope.
     */
    async "Runtime.getProperties"({ objectId, accessorPropertiesOnly, generatePreview }) {
        if (accessorPropertiesOnly === true) {
            return { result: [] };
        }

        // a scope: its variables
        if (objectId.startsWith("scope:")) {
            const variables = await this.scopeVariables(objectId);
            const result = variables.map(variable => ({
                name: variable.name, value: remoteObject(variable.value),
                writable: true, configurable: true, enumerable: true, isOwn: true,
            }));

            await this.addPreviews(result.map(descriptor => descriptor.value), generatePreview);
            return { result };
        }

        // an object of the device: its own properties (getters never called)
        const session = await this.ensureSession();
        const { properties, internals } = await session.properties(handleOf(objectId));
        const result = properties.map(property => propertyDescriptor(property));
        const internalProperties = internals.map(({ name, value }) => ({ name, value: remoteObject(value) }));
        await this.addPreviews(result.map(descriptor => descriptor.value).filter(value => value !== undefined), generatePreview);
        return { result, internalProperties };
    },

    /**
     * Frees an object of the device kept by its handle.
     */
    async "Runtime.releaseObject"({ objectId }) {
        const isDeviceObject = objectId.startsWith("h:") && this.session !== null;
        if (isDeviceObject) {
            await this.session.release(handleOf(objectId));
        }

        return {};
    },

    /**
     * Changes a variable of a frame of the pause; its scopes are read again.
     */
    async "Debugger.setVariableValue"({ callFrameId, variableName, newValue }) {
        const level = this.levelOf(callFrameId);
        await this.session.setVariable(level, variableName, fromCallArgument(newValue));
        if (this.pause !== null) {
            this.pause.scopes.delete(level);
        }

        return {};
    },
};

// ### Values ##################################################################

/**
 * A value of the session (decodeValue of qjsx:debugsession) as a CDP
 * RemoteObject.
 */
export function remoteObject(value) {
    switch (value.type) {
        case "undefined": {
            return { type: "undefined" };
        }
        case "null": {
            return { type: "object", subtype: "null", value: null };
        }
        case "number": {
            return numberObject(value.value);
        }
        case "boolean":
        case "string": {
            return { type: value.type, value: value.value };
        }
        case "bigint": {
            return { type: "bigint", unserializableValue: `${value.value}n`, description: `${value.value}n` };
        }
        case "symbol": {
            return { type: "symbol", objectId: `h:${value.handle}`, description: value.description };
        }
        case "uninitialized": {
            return { type: "undefined", description: "<uninitialized>" };
        }
        case "object": {
            return objectRemote(value);
        }
        default: {
            return { type: "undefined" };
        }
    }
}

/**
 * A number: NaN, the infinities and -0 cannot be JSON values.
 */
function numberObject(number) {
    // the text of a number JSON cannot hold
    let special;
    if (Number.isNaN(number)) {
        special = "NaN";
    } else if (number === Infinity) {
        special = "Infinity";
    } else if (number === -Infinity) {
        special = "-Infinity";
    } else if (Object.is(number, -0)) {
        special = "-0";
    }

    if (special !== undefined) {
        return { type: "number", unserializableValue: special, description: special };
    }

    return { type: "number", value: number, description: String(number) };
}

/**
 * An object: by handle (a function, or an object with a subtype), or by
 * value (returnByValue, or an error made on the host without handle).
 */
function objectRemote(value) {
    // by value
    if (value.handle === undefined) {
        const remote = { type: "object", className: value.className, description: value.description };
        if (value.kind === "error") {
            remote.subtype = "error";
        }

        if ("value" in value) {
            remote.value = value.value;
        }

        return remote;
    }

    // by handle, with its subtype if CDP has one
    const remote = {
        type: value.kind === "function" ? "function" : "object",
        className: value.className,
        description: value.description,
        objectId: `h:${value.handle}`,
    };

    const subtype = SUBTYPES[value.kind];
    if (subtype !== undefined) {
        remote.subtype = subtype;
    }

    return remote;
}

/**
 * The text of a value in an object preview: a primitive as text (cut), an
 * object by its description.
 */
function previewText(remote) {
    // the text of the value
    let text;
    if (remote.unserializableValue !== undefined) {
        text = remote.unserializableValue;
    } else if (remote.subtype === "null") {
        text = "null";
    } else if (remote.type === "undefined") {
        text = "undefined";
    } else if (remote.value !== undefined && typeof remote.value !== "object") {
        text = String(remote.value);
    } else {
        text = remote.description ?? "";
    }

    // cut when too long
    return text.length > PREVIEW_TEXT ? `${text.slice(0, PREVIEW_TEXT)}…` : text;
}

/**
 * A property of qjsx:debugsession properties() as a CDP PropertyDescriptor.
 */
function propertyDescriptor(property) {
    // a symbol is named by its description
    const name = typeof property.name === "string" ? property.name : property.name.description;
    const descriptor = {
        name,
        configurable: property.configurable,
        enumerable: property.enumerable,
        isOwn: true,
    };

    // its getter and setter, or its value
    if (property.accessor) {
        descriptor.get = remoteObject(property.getter);
        descriptor.set = remoteObject(property.setter);
    } else {
        descriptor.value = remoteObject(property.value);
        descriptor.writable = property.writable;
    }

    // the symbol itself
    if (typeof property.name !== "string") {
        descriptor.symbol = remoteObject(property.name);
    }

    return descriptor;
}

/**
 * The result of an evaluation or a call as CDP gives it.
 */
function evaluationResult(answer) {
    if (answer.exception === undefined) {
        return { result: remoteObject(answer.value) };
    }

    // an exception: the result, and its details
    const exception = remoteObject(answer.exception);
    return {
        result: exception,
        exceptionDetails: {
            exceptionId: 1,
            text: "Uncaught",
            lineNumber: 0,
            columnNumber: 0,
            exception,
        },
    };
}

/**
 * The handle of an objectId "h:<handle>".
 */
function handleOf(objectId) {
    if (!objectId.startsWith("h:")) {
        throw new CdpError(`unknown object ${objectId}`);
    }

    return Number(objectId.slice(2));
}

/**
 * An object of the device known by its objectId, as a value for the session.
 */
function valueOfObjectId(objectId) {
    return { type: "object", handle: handleOf(objectId) };
}

/**
 * A CDP CallArgument as a value for the session.
 */
function fromCallArgument(argument) {
    if (argument === undefined) {
        return undefined;
    }

    if (argument.objectId !== undefined) {
        return valueOfObjectId(argument.objectId);
    }

    // "123n" is a bigint; "NaN", "Infinity", "-0"... numbers
    if (argument.unserializableValue !== undefined) {
        const text = argument.unserializableValue;
        const isBigInt = text.endsWith("n");
        return isBigInt ? BigInt(text.slice(0, -1)) : Number(text);
    }

    return argument.value;
}

// ### Files ###################################################################

/**
 * file:/// URL of an absolute path ('/' separators: "C:/x/y.js" or "/x/y.js").
 */
export function fileUrl(path) {
    const encoded = encodeURI(path);
    return path.startsWith("/") ? `file://${encoded}` : `file:///${encoded}`;
}

/**
 * True if the URL of a script matches a breakpoint of the client (its url,
 * or its urlRegex, a JS regular expression).
 */
function matchesUrl(breakpoint, url) {
    if (breakpoint.urlRegex !== undefined) {
        return new RegExp(breakpoint.urlRegex).test(url);
    }

    return breakpoint.url === url;
}

/**
 * Writes a file unless it already holds that text (folders made as needed).
 */
function writeIfChanged(path, text) {
    const same = fs.existsSync(path) && fs.readFileSync(path, "utf8") === text;
    if (same) {
        return;
    }

    fs.mkdirSync(dirname(path), { recursive: true });
    fs.writeFileSync(path, text);
}
