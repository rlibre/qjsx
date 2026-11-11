/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A debugging session with a device, for the debugger proxy (no CDP here:
// the CDP server sits on top of it). It speaks the debugger requests of the
// agent protocol (qjsx:protocol) and gives back names and source lines, read
// in the debug info of the package (qjsx:debuginfo).
//
//   const session = new DebugSession(device, index, { onPaused, onResumed, ... });
//   await session.attach();
//   await session.setBreakpoint("src/main.js", 12);
//   // onPaused({ reason, frames, exception, returnValue })
//   const scopes = await session.scopes(0);
//   const { value } = await session.evaluate("box.width * 2");
//   await session.stepOver();
//
// Lines and columns start at 0 (columns in UTF-16 code units), as in CDP.
//
// What the session does by itself:
//   - SCRIPT_LOADED: sets the statement stops of every function of the module
//     (pause and steps) and the breakpoints kept for it, then RESUME;
//   - STARTED: takes the package of the new build id (redeployed, restarted)
//     and resolves the kept breakpoints again by line;
//   - PAUSED: reads the stack and gives it, resolved, to onPaused; the
//     variables are read on demand (scopes()).
// Device events are handled one at a time, in order; the session takes the
// onEvent handler of the device and passes every event on to the previous
// one.
//
// Values come decoded from the protocol (decodeValue below):
//   { type: "undefined" | "null" | "boolean" | "number" | "string", value }
//   { type: "bigint", value: decimal text }
//   { type: "symbol", handle, description }
//   { type: "object", handle, kind, className, description, size, location }
//   { type: "uninitialized" }      a variable in its temporal dead zone
// Objects stay on the device: their handles live in the "pause" group
// (released when the program resumes), or in the "console" group when the
// program is not paused.
import { compile } from "sys/compiler";
import {
    KIND, KEY, VALUE, OBJECT_KIND, STEP_KIND, STOP_KIND, EXCEPTION_MODE, SLOT_KIND, GROUP, PAUSE_REASON,
    PROPERTY, encodeStops,
} from "qjsx:protocol";
import { VARIABLE_FLAG } from "qjsx:debuginfo";
import { DeviceError } from "qjsx:device";

// Name of the function made by evaluate(), as the compiler records it.
const EVALUATION_NAME = "<debugger>";

export class DebugSession {
    // device: an opened qjsx:device Device, with a session (hello) done;
    // index: a qjsx:debuginfo PackageIndex;
    // handlers: onPaused(pause), onResumed(), onScriptLoaded(module, script),
    // onStarted(pkg, buildId), onExited(code), onError(error)
    constructor(device, index, handlers = {}) {
        this.device = device;
        this.index = index;
        this.handlers = handlers;

        this.buildId = undefined;          // build id of the package of the device
        this.package = undefined;          // its DebugPackage; undefined if not found
        this.loadedModules = new Set();    // modules the running program has loaded
        this.breakpoints = new Map();      // id -> { id, module, line, column, location, number, pcs }
        this.nextBreakpointId = 1;
        this.appliedStops = new Map();     // module -> Set of the function numbers with breakpoints on the device
        this.paused = null;                // { reason, frames } while the program is paused
        this.loadedFunctions = new Map();  // callFunction(): source -> handle of the loaded function (this run)

        // device events: handled in order, then passed on to the previous handler
        this.events = Promise.resolve();
        this.previousHandlers = device.handlers;
        device.handlers = { ...device.handlers, onEvent: (kind, payload) => this.queueEvent(kind, payload) };
    }

    // ### Session #############################################################

    // Turns debugging on and takes the package installed on the device. A
    // program already running has loaded some modules: their stops are set
    // now. Returns { running, buildId, package }.
    async attach() {
        await this.device.request(KIND.DEBUG_ATTACH);

        // the installed package: the one running, or the next one to run
        const info = await this.device.request(KIND.INFO);
        const installed = info.get(KEY.BUILD_ID);
        if (installed !== null && installed !== undefined) {
            this.usePackage(installed);
        }

        // the modules already loaded by a running program (none without a
        // program: the device refuses LIST_SCRIPTS then)
        const running = info.get(KEY.RUNNING);
        const scripts = running ? await this.listScripts() : [];
        for (const module of scripts) {
            this.loadedModules.add(module);
            await this.setModuleStops(module);
        }

        return { running, buildId: this.buildId, package: this.package };
    }

    // The modules loaded by the running program; [] if it ended meanwhile.
    async listScripts() {
        try {
            return (await this.device.request(KIND.LIST_SCRIPTS)).get(KEY.SCRIPTS);
        } catch (error) {
            if (error instanceof DeviceError) {
                return [];
            }
            throw error;
        }
    }

    // Turns debugging off (stops removed, a paused program resumes) and gives
    // the device its event handler back.
    async detach() {
        await this.device.request(KIND.DEBUG_DETACH);
        this.device.handlers = this.previousHandlers;
        this.paused = null;
    }

    // Takes the package of 'buildId' and resolves the breakpoints again in it.
    usePackage(buildId) {
        this.buildId = buildId;
        this.package = this.index.find(buildId);
        for (const breakpoint of this.breakpoints.values()) {
            this.resolveBreakpoint(breakpoint);
        }
    }

    // ### Breakpoints #########################################################

    // A breakpoint at (line, column) of a module, kept by the session and set
    // again in every run. Returns { id, location }: 'location' is where it
    // really is (a line without statement slides to the next one), null
    // while the module is not in the package known yet. Returns null if the
    // module has no statement from there on.
    async setBreakpoint(module, line, column = 0) {
        const breakpoint = { id: this.nextBreakpointId++, module, line, column, location: null, number: 0, pcs: [] };
        const isResolved = this.resolveBreakpoint(breakpoint);
        const scriptKnown = this.package?.script(module) !== undefined;
        if (scriptKnown && !isResolved) {
            return null;
        }

        this.breakpoints.set(breakpoint.id, breakpoint);
        await this.applyBreakpoints(module);
        return { id: breakpoint.id, location: breakpoint.location };
    }

    // Removes a breakpoint (no-op for an unknown id).
    async removeBreakpoint(id) {
        const breakpoint = this.breakpoints.get(id);
        if (breakpoint === undefined) {
            return;
        }

        this.breakpoints.delete(id);
        await this.applyBreakpoints(breakpoint.module);
    }

    // { module, location } of a kept breakpoint ('location' null while it is
    // not resolved), or undefined for an unknown id.
    breakpointLocation(id) {
        const breakpoint = this.breakpoints.get(id);
        if (breakpoint === undefined) {
            return undefined;
        }
        return { module: breakpoint.module, location: breakpoint.location };
    }

    // Finds where a breakpoint goes in the current package. False if its
    // module is unknown or has no statement from its line on.
    resolveBreakpoint(breakpoint) {
        const found = this.package?.script(breakpoint.module)?.breakpointAt(breakpoint.line, breakpoint.column);
        if (found === undefined || found === null) {
            breakpoint.location = null;
            breakpoint.pcs = [];
            return false;
        }

        breakpoint.location = { line: found.line, column: found.column };
        breakpoint.number = found.number;
        breakpoint.pcs = found.pcs;
        return true;
    }

    // Sends the breakpoints of a loaded module to the device. SET_STOPS
    // replaces the breakpoints of the functions it lists: the functions that
    // had some and have none now are listed too, with no offset.
    async applyBreakpoints(module) {
        if (!this.loadedModules.has(module)) {
            return;
        }

        // the offsets per function, from the kept breakpoints of the module
        const offsetsByFunction = new Map();
        for (const breakpoint of this.breakpoints.values()) {
            if (breakpoint.module !== module || breakpoint.location === null) {
                continue;
            }
            const offsets = offsetsByFunction.get(breakpoint.number) ?? new Set();
            breakpoint.pcs.forEach(pc => offsets.add(pc));
            offsetsByFunction.set(breakpoint.number, offsets);
        }

        // these functions, and those that had breakpoints before
        const previous = this.appliedStops.get(module) ?? new Set();
        const numbers = new Set([...previous, ...offsetsByFunction.keys()]);
        if (numbers.size === 0) {
            return;
        }

        const functions = [...numbers].map(number => ({ number, offsets: [...(offsetsByFunction.get(number) ?? [])] }));
        await this.device.request(KIND.SET_STOPS, {
            [KEY.MODULE]: module, [KEY.STOP_KIND]: STOP_KIND.BREAKPOINTS, [KEY.DATA]: encodeStops(functions),
        });
        this.appliedStops.set(module, new Set(offsetsByFunction.keys()));
    }

    // Statement stops (pause and steps) and breakpoints of a loaded module.
    // Nothing for a module the package does not know.
    async setModuleStops(module) {
        const script = this.package?.script(module);
        if (script === undefined) {
            return;
        }

        await this.device.request(KIND.SET_STOPS, {
            [KEY.MODULE]: module, [KEY.STOP_KIND]: STOP_KIND.STATEMENTS, [KEY.DATA]: encodeStops(script.statementStops()),
        });
        await this.applyBreakpoints(module);
    }

    // ### Execution ###########################################################

    // When to pause on a thrown value: "none", "uncaught" or "all".
    async setPauseOnExceptions(mode) {
        const modes = { none: EXCEPTION_MODE.NONE, uncaught: EXCEPTION_MODE.UNCAUGHT, all: EXCEPTION_MODE.ALL };
        if (modes[mode] === undefined) {
            throw new Error(`pause on exceptions: unknown mode '${mode}'`);
        }
        await this.device.request(KIND.SET_EXCEPTIONS, { [KEY.MODE]: modes[mode] });
    }

    async pause() {
        await this.device.request(KIND.PAUSE);
    }

    async resume() {
        await this.device.request(KIND.RESUME);
    }

    async stepInto() {
        await this.device.request(KIND.STEP, { [KEY.STEP]: STEP_KIND.INTO });
    }

    async stepOver() {
        await this.device.request(KIND.STEP, { [KEY.STEP]: STEP_KIND.OVER });
    }

    async stepOut() {
        await this.device.request(KIND.STEP, { [KEY.STEP]: STEP_KIND.OUT });
    }

    // ### Device events #######################################################

    // Passes the event on to the previous handler, and queues it here.
    queueEvent(kind, payload) {
        this.previousHandlers.onEvent?.(kind, payload);
        this.events = this.events
            .then(() => this.handleEvent(kind, payload))
            .catch(error => this.handlers.onError?.(error));
    }

    // One device event. The handlers of the user are called, not awaited: they
    // may step or resume, which brings the next events.
    async handleEvent(kind, payload) {
        switch (kind) {
        case KIND.SCRIPT_LOADED: {
            // the module waits for RESUME: set its stops first (RESUME even
            // if that fails, else the program would wait for ever)
            const module = payload.get(KEY.MODULE);
            this.loadedModules.add(module);
            try {
                await this.setModuleStops(module);
            } finally {
                await this.device.request(KIND.RESUME);
            }
            this.handlers.onScriptLoaded?.(module, this.package?.script(module));
            break;
        }
        case KIND.STARTED: {
            // a new run: nothing loaded yet, maybe another package
            this.loadedModules.clear();
            this.appliedStops.clear();
            this.loadedFunctions.clear();
            this.usePackage(payload.get(KEY.BUILD_ID));
            this.handlers.onStarted?.(this.package, this.buildId);
            break;
        }
        case KIND.EXITED: {
            this.loadedModules.clear();
            this.appliedStops.clear();
            this.loadedFunctions.clear();
            this.paused = null;
            this.handlers.onExited?.(payload.get(KEY.CODE));
            break;
        }
        case KIND.PAUSED: {
            // the stack, resolved, then the user
            const frames = await this.readStack();
            const reasonName = Object.keys(PAUSE_REASON).find(name => PAUSE_REASON[name] === payload.get(KEY.REASON));
            const reason = reasonName?.toLowerCase() ?? "unknown";
            this.paused = { reason, frames };

            const pause = { reason, frames };
            if (payload.has(KEY.EXCEPTION)) {
                pause.exception = decodeValue(payload.get(KEY.EXCEPTION));
                pause.uncaught = payload.get(KEY.UNCAUGHT) === true;
            }
            if (payload.has(KEY.RETURN_VALUE)) {
                pause.returnValue = decodeValue(payload.get(KEY.RETURN_VALUE));
            }
            this.handlers.onPaused?.(pause);
            break;
        }
        case KIND.RESUMED: {
            this.paused = null;
            this.handlers.onResumed?.();
            break;
        }
        default: {
            break;
        }
        }
    }

    // The stack of the pause, level 0 first:
    //   { level, id, native, module, number, pc, functionName, line, column, script }
    // A native frame has native true and no position; a frame of a module
    // the package does not know has no line.
    async readStack() {
        const frames = (await this.device.request(KIND.GET_STACK)).get(KEY.FRAMES);
        return frames.map(([id, module, number, pc], level) => {
            if (module === null) {
                return { level, id, native: true, module: null, functionName: "(native)" };
            }

            // the level 0 stops at its instruction; the others are after a call
            const script = this.package?.script(module);
            const location = script?.location(number, pc, { exact: level === 0 });
            return {
                level, id, native: false, module, number, pc,
                functionName: script?.functionName(number) ?? `(function ${number})`,
                line: location?.line,
                column: location?.column,
                script,
            };
        });
    }

    // ### Variables ###########################################################

    // The frame at 'level' of the pause; throws if the program is not paused
    // or the frame has no debug info.
    pausedFrame(level) {
        if (this.paused === null) {
            throw new Error("the program is not paused");
        }

        const frame = this.paused.frames[level];
        if (frame === undefined) {
            throw new Error(`no frame at level ${level}`);
        }
        if (frame.native || frame.script === undefined) {
            throw new Error(`no debug info for the frame at level ${level}`);
        }
        return frame;
    }

    // The scopes of the variables visible in a frame (see
    // ScriptInfo.scopesAt), with the descriptions of the slots.
    scopesOf(frame) {
        return frame.script.scopesAt(frame.number, frame.pc, { exact: frame.level === 0 });
    }

    // The values of the slots of a frame, as sent by the device (not
    // decoded): { [slotKind]: [value] } for the kinds asked.
    async readSlots(level, slotKinds) {
        const values = {};
        for (const slotKind of slotKinds) {
            const answer = await this.device.request(KIND.GET_SLOTS, {
                [KEY.LEVEL]: level, [KEY.SLOT_KIND]: slotKind, [KEY.GROUP]: GROUP.PAUSE,
            });
            values[slotKind] = answer.get(KEY.VALUES);
        }
        return values;
    }

    // The variables of a frame of the pause, by scope, innermost first:
    //   [{ kind: "block" | "local" | "closure", variables: [{ name, value }] }]
    async scopes(level) {
        const frame = this.pausedFrame(level);
        const scopes = this.scopesOf(frame);

        // read the slots of the kinds these variables use
        const slotKinds = new Set(scopes.flatMap(scope => scope.variables.map(variable => variable.slotKind)));
        const values = await this.readSlots(level, slotKinds);

        return scopes.map(scope => ({
            kind: scope.kind,
            variables: scope.variables.map(variable => ({
                name: variable.name,
                value: decodeValue(values[variable.slotKind][variable.index]),
            })),
        }));
    }

    // The value of 'this' in a frame of the pause, or undefined when the
    // function has no 'this' slot (QuickJS makes one only when the function
    // uses 'this').
    async thisOf(level) {
        const frame = this.pausedFrame(level);
        const slot = frame.script.thisSlot(frame.number);
        if (slot === null) {
            return undefined;
        }

        const values = await this.readSlots(level, [slot.slotKind]);
        return decodeValue(values[slot.slotKind][slot.index]);
    }

    // The visible variable 'name' of a frame (the innermost one), or undefined.
    findVariable(frame, name) {
        for (const scope of this.scopesOf(frame)) {
            const variable = scope.variables.find(candidate => candidate.name === name);
            if (variable !== undefined) {
                return variable;
            }
        }
        return undefined;
    }

    // Changes a variable of a frame of the pause. 'value': a primitive, or a
    // value given by the session (an object by its handle).
    async setVariable(level, name, value) {
        const frame = this.pausedFrame(level);
        const variable = this.findVariable(frame, name);
        if (variable === undefined) {
            throw new Error(`no variable '${name}' here`);
        }
        if (variable.flags & VARIABLE_FLAG.CONST) {
            throw new Error(`'${name}' is a constant`);
        }

        await this.device.request(KIND.SET_SLOT, {
            [KEY.LEVEL]: level, [KEY.SLOT_KIND]: variable.slotKind, [KEY.INDEX]: variable.index, [KEY.VALUE]: encodeValue(value),
        });
    }

    // Own properties of an object of the device:
    //   { properties: [{ name, writable, enumerable, configurable, accessor,
    //     value, getter, setter }], internals: [{ name, value }] }
    // 'name' is a text, or a symbol value. Getters are never called.
    async properties(handle) {
        const group = this.paused !== null ? GROUP.PAUSE : GROUP.CONSOLE;
        const answer = await this.device.request(KIND.GET_PROPERTIES, { [KEY.HANDLE]: handle, [KEY.GROUP]: group });

        const properties = answer.get(KEY.PROPERTIES).map(([name, flags, value, getter, setter]) => {
            const isAccessor = (flags & PROPERTY.ACCESSOR) !== 0;
            return {
                name: typeof name === "string" ? name : decodeValue(name),
                writable: (flags & PROPERTY.WRITABLE) !== 0,
                enumerable: (flags & PROPERTY.ENUMERABLE) !== 0,
                configurable: (flags & PROPERTY.CONFIGURABLE) !== 0,
                accessor: isAccessor,
                value: isAccessor ? undefined : decodeValue(value),
                getter: isAccessor ? decodeValue(getter) : undefined,
                setter: isAccessor ? decodeValue(setter) : undefined,
            };
        });
        const internals = answer.get(KEY.INTERNALS).map(([name, value]) => ({ name, value: decodeValue(value) }));
        return { properties, internals };
    }

    // Frees an object of the device kept by its handle (no-op for an unknown
    // one).
    async release(handle) {
        await this.device.request(KIND.RELEASE, { [KEY.HANDLE]: handle });
    }

    // ### Evaluation ##########################################################

    // Evaluates an expression on the device: { value } or { exception }.
    // Paused: in the frame at 'level', which sees the variables of that
    // frame. Wrapped in a function whose parameters are these variables (the
    // innermost of a name), compiled here, loaded and called on the device
    // with their values and 'this'. The parameters are copies: an assignment
    // does not change the variable of the program (setVariable does).
    // Not paused: in the global scope.
    // 'returnByValue': the result comes back as a plain JS value (through
    // JSON on the device: { type, value }), not as a handle.
    async evaluate(expression, { level = 0, returnByValue = false } = {}) {
        const isPaused = this.paused !== null;
        const group = isPaused ? GROUP.PAUSE : GROUP.CONSOLE;

        // the variables of the frame and 'this', with their values
        let names = [];
        let argumentValues = [];
        let thisValue = undefined;
        if (isPaused) {
            const frame = this.pausedFrame(level);
            const parameters = this.parametersOf(frame);
            const thisSlot = frame.script.thisSlot(frame.number);

            const slotKinds = new Set(parameters.map(variable => variable.slotKind));
            if (thisSlot !== null) {
                slotKinds.add(thisSlot.slotKind);
            }
            const values = await this.readSlots(level, slotKinds);

            // a variable in its temporal dead zone cannot be passed
            const usable = parameters.filter(variable => !isUninitialized(values[variable.slotKind][variable.index]));
            names = usable.map(variable => variable.name);
            argumentValues = usable.map(variable => toDevice(values[variable.slotKind][variable.index]));
            if (thisSlot !== null) {
                thisValue = toDevice(values[thisSlot.slotKind][thisSlot.index]);
            }
        }

        // compile the wrapper here: a syntax error never reaches the device
        // (the line breaks keep a trailing '//' comment out of the code)
        const result = returnByValue ? `JSON.stringify((${expression}\n))` : `(${expression}\n)`;
        const source = `(function (${names.join(", ")}) {\nreturn ${result};\n})`;
        let bytecode;
        try {
            bytecode = compile(source, EVALUATION_NAME, { module: false, strip: "all" }).bytecode;
        } catch (error) {
            return { exception: localError(error) };
        }

        // load it, then call it with the values
        const loaded = (await this.device.request(KIND.LOAD_FUNCTION, { [KEY.DATA]: bytecode, [KEY.GROUP]: group })).get(KEY.VALUE);
        const answer = await this.callLoaded(loaded[1], thisValue, argumentValues, group);
        return returnByValue ? byValue(answer) : answer;
    }

    // Calls a function given by its source text (a function expression, as
    // in CDP's Runtime.callFunctionOn) on the device: { value } or
    // { exception }. 'thisValue' and 'args': values given by the session, or
    // primitives. The function is compiled and loaded once per source text,
    // and kept (persistent group) until the run ends. 'returnByValue': as in
    // evaluate().
    async callFunction(declaration, { thisValue, args = [], returnByValue = false } = {}) {
        // the function on the device: loaded before, or compiled and loaded now
        const cacheKey = `${returnByValue ? "json" : "value"}:${declaration}`;
        let handle = this.loadedFunctions.get(cacheKey);
        if (handle === undefined) {
            const called = `(${declaration}\n).apply(this, arguments)`;
            const result = returnByValue ? `JSON.stringify(${called})` : called;
            let bytecode;
            try {
                bytecode = compile(`(function () {\nreturn ${result};\n})`, EVALUATION_NAME, { module: false, strip: "all" }).bytecode;
            } catch (error) {
                return { exception: localError(error) };
            }

            const loaded = await this.device.request(KIND.LOAD_FUNCTION, { [KEY.DATA]: bytecode, [KEY.GROUP]: GROUP.PERSISTENT });
            handle = loaded.get(KEY.VALUE)[1];
            this.loadedFunctions.set(cacheKey, handle);
        }

        // call it
        const group = this.paused !== null ? GROUP.PAUSE : GROUP.CONSOLE;
        const answer = await this.callLoaded(handle, encodeValue(thisValue), args.map(encodeValue), group);
        return returnByValue ? byValue(answer) : answer;
    }

    // CALL of a function loaded on the device: { value } or { exception },
    // decoded.
    async callLoaded(handle, thisValue, argumentValues, group) {
        const answer = await this.device.request(KIND.CALL, {
            [KEY.FUNCTION]: [VALUE.HANDLE, handle],
            [KEY.THIS]: thisValue,
            [KEY.ARGUMENTS]: argumentValues,
            [KEY.GROUP]: group,
        });

        if (answer.has(KEY.EXCEPTION)) {
            return { exception: decodeValue(answer.get(KEY.EXCEPTION)) };
        }
        return { value: decodeValue(answer.get(KEY.VALUE)) };
    }

    // The variables visible in a frame, one per name (the innermost scope
    // wins), as parameters of an evaluation.
    parametersOf(frame) {
        const byName = new Map();
        for (const scope of this.scopesOf(frame)) {
            for (const variable of scope.variables) {
                if (!byName.has(variable.name)) {
                    byName.set(variable.name, variable);
                }
            }
        }
        return [...byName.values()];
    }
}

// ### Values ##################################################################

// A value sent by the device (protocol encoding, see qjsx:protocol) as a JS
// record (see the top of this file).
export function decodeValue(raw) {
    if (raw === null) {
        return { type: "null", value: null };
    }

    if (!Array.isArray(raw)) {
        return { type: typeof raw, value: raw };
    }

    const tag = raw[0];
    switch (tag) {
    case VALUE.OBJECT: {
        const [, handle, kind, className, description, size, location] = raw;
        return {
            type: "object",
            handle,
            kind: objectKindName(kind),
            className,
            description,
            size: size >= 0 ? size : undefined,
            location: Array.isArray(location) ? { module: location[0], number: location[1] } : null,
        };
    }
    case VALUE.SYMBOL: {
        return { type: "symbol", handle: raw[1], description: raw[2] };
    }
    case VALUE.BIGINT: {
        return { type: "bigint", value: raw[1] };
    }
    case VALUE.UNINITIALIZED: {
        return { type: "uninitialized" };
    }
    default: {
        return { type: "unknown", value: raw };
    }
    }
}

// Name of an OBJECT_KIND number: "plain", "function", "array"...
function objectKindName(kind) {
    const name = Object.keys(OBJECT_KIND).find(key => OBJECT_KIND[key] === kind);
    return name?.toLowerCase() ?? "unknown";
}

// The answer of a call made with JSON.stringify around the result, back to
// a plain value: { value: { type, value } }. An exception stays as it is.
function byValue(answer) {
    if (answer.exception !== undefined) {
        return answer;
    }

    // JSON.stringify gives undefined for undefined and functions
    const text = answer.value.value;
    if (answer.value.type !== "string") {
        return { value: { type: "undefined", value: undefined } };
    }

    const parsed = JSON.parse(text);
    const type = parsed === null ? "null" : typeof parsed;
    return { value: { type, value: parsed } };
}

function isUninitialized(raw) {
    return Array.isArray(raw) && raw[0] === VALUE.UNINITIALIZED;
}

// A value sent by the device, ready to be sent back: an object or a symbol
// by its handle, the rest as it came.
function toDevice(raw) {
    const isKept = Array.isArray(raw) && (raw[0] === VALUE.OBJECT || raw[0] === VALUE.SYMBOL);
    return isKept ? [VALUE.HANDLE, raw[1]] : raw;
}

// A value given by the user of the session, in the protocol encoding: a
// decoded value (from this session) or a plain primitive.
function encodeValue(value) {
    if (typeof value === "bigint") {
        return [VALUE.BIGINT, String(value)];
    }

    const isDecoded = value !== null && typeof value === "object" && typeof value.type === "string";
    if (!isDecoded) {
        return value;
    }

    switch (value.type) {
    case "object":
    case "symbol": {
        return [VALUE.HANDLE, value.handle];
    }
    case "bigint": {
        return [VALUE.BIGINT, value.value];
    }
    case "uninitialized": {
        throw new Error("an uninitialized value cannot be sent");
    }
    default: {
        return value.value;
    }
    }
}

// An error raised here (syntax error of an evaluated expression), shaped like
// an error object of the device, without handle.
function localError(error) {
    const className = error.name ?? "Error";
    return {
        type: "object",
        handle: undefined,
        kind: "error",
        className,
        description: `${className}: ${error.message}`,
        size: undefined,
        location: null,
    };
}
