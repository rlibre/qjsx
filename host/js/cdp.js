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
import * as fs from "fs";
import { dirname, resolve } from "stdlib/path";
import { qjsxVersion } from "compiler";
import { startServer } from "qjsx:websocket";
import { DebugSession } from "qjsx:debugsession";

export const DEFAULT_PORT = 9339;
const TARGET_ID = "qjsx";
const CONTEXT_ID = 1;

// JSON-RPC error codes of CDP.
const METHOD_NOT_FOUND = -32601;
const SERVER_ERROR = -32000;

// Methods accepted and ignored: js-debug sends them, nothing here needs them.
const IGNORED_METHODS = new Set([
    "Runtime.runIfWaitingForDebugger", "Profiler.enable", "Profiler.disable", "NodeWorker.enable",
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

class CdpError extends Error {
    constructor(message, code = SERVER_ERROR) {
        super(message);
        this.code = code;
    }
}

export class CdpServer {
    // device: an opened Device (hello done); index: a PackageIndex;
    // host, port: where to listen; title: shown by the debugger;
    // sourceCache: folder where the sources of the library modules are
    // written (js-debug opens files); onStatus(text): messages for the user
    constructor({ device, index, host = "127.0.0.1", port = DEFAULT_PORT, title = "qjsx", sourceCache, onStatus }) {
        this.device = device;
        this.index = index;
        this.host = host;
        this.listenPort = port;
        this.title = title;
        this.sourceCache = sourceCache;
        this.onStatus = onStatus ?? (() => {});

        this.server = null;
        this.client = null;            // WebSocket of the debugger; null: none
        this.resetClientState();
    }

    // What belongs to one client: forgotten when it leaves.
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

    // Starts listening; throws if the port is taken.
    start() {
        this.server = startServer({
            host: this.host,
            port: this.listenPort,
            routes: path => this.route(path),
            acceptsSocket: path => path === `/${TARGET_ID}`,
            onSocket: socket => this.onSocket(socket),
        });
    }

    port() {
        return this.server.port();
    }

    webSocketUrl() {
        return `ws://${this.host}:${this.port()}/${TARGET_ID}`;
    }

    // The HTTP answers of the inspector protocol.
    route(path) {
        if (path === "/json/version") {
            return { Browser: `qjsx/${qjsxVersion}`, "Protocol-Version": "1.3" };
        }

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

    // A debugger connects: it replaces the previous one.
    onSocket(socket) {
        if (this.client !== null) {
            const previous = this.client;
            this.endClient(previous);
            previous.close();
        }

        this.client = socket;
        socket.onMessage(text => this.onClientMessage(socket, text));
        socket.onClose(() => this.endClient(socket));
        this.onStatus("debugger connected");
    }

    // The client left (or was replaced): the device goes on without debugger.
    endClient(socket) {
        if (this.client !== socket) {
            return;
        }

        const session = this.session;
        this.client = null;
        this.resetClientState();
        this.onStatus("debugger disconnected");
        session?.detach().catch(error => this.onStatus(`detach: ${error.message}`));
    }

    async close() {
        if (this.client !== null) {
            this.client.close();
        }
        this.server?.close();
    }

    // ### Messages ############################################################

    // One command of the client: its answer, or an error.
    async onClientMessage(socket, text) {
        let message;
        try {
            message = JSON.parse(text);
        } catch (error) {
            return;
        }

        const { id, method, params } = message;
        try {
            const result = await this.handle(method, params ?? {});
            this.sendTo(socket, { id, result: result ?? {} });
        } catch (error) {
            const code = error instanceof CdpError ? error.code : SERVER_ERROR;
            this.sendTo(socket, { id, error: { code, message: error.message } });
        }
    }

    sendTo(socket, message) {
        if (socket === this.client) {
            socket.send(JSON.stringify(message));
        }
    }

    // An event to the client, if there is one.
    event(method, params) {
        if (this.client !== null) {
            this.client.send(JSON.stringify({ method, params }));
        }
    }

    // ### Program output ######################################################

    // Output of the program of the device: to the debug console of the
    // client (one message per line), else to this terminal.
    output(stream, text) {
        if (this.client === null || !this.runtimeEnabled) {
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

    consoleMessage(type, text) {
        this.event("Runtime.consoleAPICalled", {
            type,
            args: [{ type: "string", value: text }],
            executionContextId: CONTEXT_ID,
            timestamp: Date.now(),
        });
    }

    // ### Session #############################################################

    // The debugging session of the client, attached on first need.
    async ensureSession() {
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

    // A new run: new scripts, the breakpoints bound again in its package.
    onStarted() {
        this.runNumber++;
        this.scriptIds.clear();
        this.scriptsById.clear();
        this.urls.clear();
        this.pause = null;

        if (this.runtimeEnabled) {
            this.event("Runtime.executionContextsCleared", {});
            this.announceContext();
        }

        for (const breakpoint of this.breakpoints.values()) {
            this.bindBreakpoint(breakpoint).catch(error => this.onStatus(`breakpoint: ${error.message}`));
        }
    }

    onExited(code) {
        this.pause = null;
        if (this.runtimeEnabled) {
            this.consoleMessage("info", `qjsx: program ended (code ${code})`);
        }
        this.onStatus(`program ended (code ${code})`);
    }

    announceContext() {
        this.event("Runtime.executionContextCreated", {
            context: { id: CONTEXT_ID, origin: "", name: this.title, uniqueId: `qjsx-${this.runNumber}`, auxData: { isDefault: true } },
        });
    }

    // ### Scripts #############################################################

    // file:/// URL of a script: its file in the project, or for a library
    // module a copy of its source in the cache folder (js-debug opens files).
    urlOf(script) {
        const known = this.urls.get(script.module);
        if (known !== undefined) {
            return known;
        }

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

    // Debugger.scriptParsed for a loaded module (once per run), then the
    // breakpoints that resolve in it.
    announceScript(module) {
        const script = this.session?.package?.script(module);
        if (!this.debuggerEnabled || script === undefined || this.scriptIds.has(module)) {
            return;
        }

        const scriptId = String(this.nextScriptId++);
        const url = this.urlOf(script);
        this.scriptIds.set(module, scriptId);
        this.scriptsById.set(scriptId, { module, script, url });

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

        for (const breakpoint of this.breakpoints.values()) {
            const location = this.locationOf(breakpoint, module);
            if (location !== null) {
                this.event("Debugger.breakpointResolved", { breakpointId: breakpoint.id, location });
            }
        }
    }

    // The script of a scriptId given by the client.
    scriptOf(scriptId) {
        const entry = this.scriptsById.get(scriptId);
        if (entry === undefined) {
            throw new CdpError(`no script ${scriptId}`);
        }
        return entry;
    }

    // ### Breakpoints #########################################################

    // Sets a breakpoint of the session in every module of the package whose
    // URL the CDP breakpoint matches and that has none yet.
    async bindBreakpoint(breakpoint) {
        const pkg = this.session?.package;
        if (pkg === undefined) {
            return;
        }

        for (const script of pkg.scripts()) {
            if (breakpoint.bindings.has(script.module) || !matchesUrl(breakpoint, this.urlOf(script))) {
                continue;
            }

            const set = await this.session.setBreakpoint(script.module, breakpoint.line, breakpoint.column);
            if (set !== null) {
                breakpoint.bindings.set(script.module, set.id);
            }
        }
    }

    // CDP location of a breakpoint in a module announced to the client, or null.
    locationOf(breakpoint, module) {
        const sessionId = breakpoint.bindings.get(module);
        const scriptId = this.scriptIds.get(module);
        const found = sessionId === undefined ? undefined : this.session.breakpointLocation(sessionId);
        if (scriptId === undefined || found?.location === null || found === undefined) {
            return null;
        }
        return { scriptId, lineNumber: found.location.line, columnNumber: found.location.column };
    }

    // The CDP locations of a breakpoint in every announced module.
    locationsOf(breakpoint) {
        return [...breakpoint.bindings.keys()]
            .map(module => this.locationOf(breakpoint, module))
            .filter(location => location !== null);
    }

    // A breakpoint of the client: refused with a condition (conditional
    // breakpoints and logpoints come with step 4), bound to the modules it
    // matches.
    async addBreakpoint({ url, urlRegex, line, column, condition }) {
        if (condition !== undefined && condition !== "") {
            throw new CdpError("conditional breakpoints and logpoints are not supported yet");
        }

        await this.ensureSession();
        const breakpoint = { id: `bp:${this.nextBreakpointId++}`, url, urlRegex, line, column, bindings: new Map() };
        this.breakpoints.set(breakpoint.id, breakpoint);
        await this.bindBreakpoint(breakpoint);
        return breakpoint;
    }

    // The breakpoints of the client hit at the pause: those bound at the
    // line of the top frame.
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

    onPaused(pause) {
        this.pauseNumber++;
        this.pause = { number: this.pauseNumber, frames: pause.frames, scopes: new Map() };

        // the scripts of the stack are announced before the pause
        for (const frame of pause.frames) {
            if (!frame.native) {
                this.announceScript(frame.module);
            }
        }

        const callFrames = pause.frames
            .filter(frame => !frame.native && this.scriptIds.has(frame.module) && frame.line !== undefined)
            .map(frame => this.callFrame(frame));
        if (callFrames.length > 0 && pause.returnValue !== undefined) {
            callFrames[0].returnValue = remoteObject(pause.returnValue);
        }

        const params = { callFrames, reason: pause.reason === "exception" ? "exception" : "other" };
        if (pause.exception !== undefined) {
            params.data = remoteObject(pause.exception);
        }
        if (pause.reason === "breakpoint" && pause.frames.length > 0) {
            params.hitBreakpoints = this.hitBreakpoints(pause.frames[0]);
        }
        this.event("Debugger.paused", params);
    }

    onResumed() {
        this.pause = null;
        this.event("Debugger.resumed", {});
    }

    // A frame of the device as a CDP call frame; the scopes are objects whose
    // properties are read on demand (Runtime.getProperties).
    callFrame(frame) {
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

        return {
            callFrameId: `${number}:${frame.level}`,
            functionName: frame.functionName,
            location: { scriptId: this.scriptIds.get(frame.module), lineNumber: frame.line, columnNumber: frame.column },
            url: this.urls.get(frame.module),
            scopeChain,
            this: { type: "undefined" },
            canBeRestarted: false,
        };
    }

    // The device level of a callFrameId of the current pause.
    levelOf(callFrameId) {
        const [number, level] = String(callFrameId).split(":").map(Number);
        if (this.pause === null || number !== this.pause.number) {
            throw new CdpError("this call frame belongs to a pause that is over");
        }
        return level;
    }

    // The variables of a scope object ("scope:<pause>:<level>:<index>").
    async scopeVariables(objectId) {
        const [, number, level, index] = objectId.split(":").map(Number);
        if (this.pause === null || number !== this.pause.number) {
            throw new CdpError("this scope belongs to a pause that is over");
        }

        let scopes = this.pause.scopes.get(level);
        if (scopes === undefined) {
            scopes = await this.session.scopes(level);
            this.pause.scopes.set(level, scopes);
        }
        return scopes[index]?.variables ?? [];
    }

    // ### Commands ############################################################

    // One CDP command: its result.
    async handle(method, params) {
        if (IGNORED_METHODS.has(method)) {
            return {};
        }

        const handler = COMMANDS[method];
        if (handler === undefined) {
            throw new CdpError(`'${method}' wasn't found`, METHOD_NOT_FOUND);
        }
        return handler.call(this, params);
    }
}

// ### Commands ################################################################

const COMMANDS = {
    "Runtime.enable"() {
        this.runtimeEnabled = true;
        // the context once the answer is gone: after the current task
        Promise.resolve().then(() => this.announceContext());
        return {};
    },

    async "Debugger.enable"() {
        this.debuggerEnabled = true;
        const session = await this.ensureSession();
        for (const module of session.loadedModules) {
            this.announceScript(module);
        }
        return { debuggerId: "qjsx" };
    },

    async "Debugger.setBreakpointByUrl"({ url, urlRegex, lineNumber, columnNumber, condition }) {
        const breakpoint = await this.addBreakpoint({ url, urlRegex, line: lineNumber, column: columnNumber ?? 0, condition });
        return { breakpointId: breakpoint.id, locations: this.locationsOf(breakpoint) };
    },

    async "Debugger.setBreakpoint"({ location, condition }) {
        const { url } = this.scriptOf(location.scriptId);
        const breakpoint = await this.addBreakpoint({ url, line: location.lineNumber, column: location.columnNumber ?? 0, condition });
        const locations = this.locationsOf(breakpoint);
        if (locations.length === 0) {
            throw new CdpError("no statement there");
        }
        return { breakpointId: breakpoint.id, actualLocation: locations[0] };
    },

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

    "Debugger.getPossibleBreakpoints"({ start, end }) {
        const { script } = this.scriptOf(start.scriptId);
        const found = script.breakableLocations(start.lineNumber, start.columnNumber ?? 0, end?.lineNumber, end?.columnNumber ?? 0);
        return {
            locations: found.map(({ line, column }) => ({ scriptId: start.scriptId, lineNumber: line, columnNumber: column, type: "break" })),
        };
    },

    "Debugger.getScriptSource"({ scriptId }) {
        return { scriptSource: this.scriptOf(scriptId).script.source };
    },

    async "Debugger.setPauseOnExceptions"({ state }) {
        const session = await this.ensureSession();
        await session.setPauseOnExceptions(state === "caught" ? "all" : state);
        return {};
    },

    async "Debugger.pause"() {
        await (await this.ensureSession()).pause();
        return {};
    },

    async "Debugger.resume"() {
        await (await this.ensureSession()).resume();
        return {};
    },

    async "Debugger.stepOver"() {
        await (await this.ensureSession()).stepOver();
        return {};
    },

    async "Debugger.stepInto"() {
        await (await this.ensureSession()).stepInto();
        return {};
    },

    async "Debugger.stepOut"() {
        await (await this.ensureSession()).stepOut();
        return {};
    },

    async "Debugger.evaluateOnCallFrame"({ callFrameId, expression, returnByValue }) {
        const level = this.levelOf(callFrameId);
        const answer = await this.session.evaluate(expression, { level, returnByValue: returnByValue === true });
        return evaluationResult(answer);
    },

    async "Runtime.evaluate"({ expression, returnByValue }) {
        const session = await this.ensureSession();
        const answer = await session.evaluate(expression, { level: 0, returnByValue: returnByValue === true });
        return evaluationResult(answer);
    },

    async "Runtime.callFunctionOn"({ functionDeclaration, objectId, arguments: callArguments, returnByValue }) {
        const session = await this.ensureSession();
        const thisValue = objectId === undefined || objectId.startsWith("scope:") ? undefined : valueOfObjectId(objectId);
        const args = (callArguments ?? []).map(fromCallArgument);
        const answer = await session.callFunction(functionDeclaration, { thisValue, args, returnByValue: returnByValue === true });
        return evaluationResult(answer);
    },

    async "Runtime.getProperties"({ objectId, accessorPropertiesOnly }) {
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
            return { result };
        }

        // an object of the device: its own properties (getters never called)
        const session = await this.ensureSession();
        const { properties, internals } = await session.properties(handleOf(objectId));
        const result = properties.map(property => propertyDescriptor(property));
        const internalProperties = internals.map(({ name, value }) => ({ name, value: remoteObject(value) }));
        return { result, internalProperties };
    },

    async "Runtime.releaseObject"({ objectId }) {
        if (objectId.startsWith("h:") && this.session !== null) {
            await this.session.release(handleOf(objectId));
        }
        return {};
    },

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

// A value of the session (decodeValue of qjsx:debugsession) as a CDP
// RemoteObject.
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

// A number: NaN, the infinities and -0 cannot be JSON values.
function numberObject(number) {
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

// An object: by handle (a function, or an object with a subtype), or by
// value (returnByValue, or an error made on the host without handle).
function objectRemote(value) {
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

// A property of qjsx:debugsession properties() as a CDP PropertyDescriptor.
function propertyDescriptor(property) {
    const name = typeof property.name === "string" ? property.name : property.name.description;
    const descriptor = {
        name,
        configurable: property.configurable,
        enumerable: property.enumerable,
        isOwn: true,
    };
    if (property.accessor) {
        descriptor.get = remoteObject(property.getter);
        descriptor.set = remoteObject(property.setter);
    } else {
        descriptor.value = remoteObject(property.value);
        descriptor.writable = property.writable;
    }
    if (typeof property.name !== "string") {
        descriptor.symbol = remoteObject(property.name);
    }
    return descriptor;
}

// The result of an evaluation or a call as CDP gives it.
function evaluationResult(answer) {
    if (answer.exception === undefined) {
        return { result: remoteObject(answer.value) };
    }

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

// The handle of an objectId "h:<handle>".
function handleOf(objectId) {
    if (!objectId.startsWith("h:")) {
        throw new CdpError(`unknown object ${objectId}`);
    }
    return Number(objectId.slice(2));
}

// An object of the device known by its objectId, as a value for the session.
function valueOfObjectId(objectId) {
    return { type: "object", handle: handleOf(objectId) };
}

// A CDP CallArgument as a value for the session.
function fromCallArgument(argument) {
    if (argument === undefined) {
        return undefined;
    }

    if (argument.objectId !== undefined) {
        return valueOfObjectId(argument.objectId);
    }

    if (argument.unserializableValue !== undefined) {
        const text = argument.unserializableValue;
        return text.endsWith("n") ? BigInt(text.slice(0, -1)) : Number(text);
    }
    return argument.value;
}

// ### Files ###################################################################

// file:/// URL of an absolute path ('/' separators: "C:/x/y.js" or "/x/y.js").
export function fileUrl(path) {
    const encoded = encodeURI(path);
    return path.startsWith("/") ? `file://${encoded}` : `file:///${encoded}`;
}

// True if the URL of a script matches a breakpoint of the client (its url,
// or its urlRegex, a JS regular expression).
function matchesUrl(breakpoint, url) {
    if (breakpoint.urlRegex !== undefined) {
        return new RegExp(breakpoint.urlRegex).test(url);
    }
    return breakpoint.url === url;
}

// Writes a file unless it already holds that text (folders made as needed).
function writeIfChanged(path, text) {
    const same = fs.existsSync(path) && fs.readFileSync(path, "utf8") === text;
    if (same) {
        return;
    }

    fs.mkdirSync(dirname(path), { recursive: true });
    fs.writeFileSync(path, text);
}
