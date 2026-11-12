/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The parts of the twin (qjsx:twin/parts): those of the board description,
// then those the project wires on it (the "twin" section of its qjsx.json),
// each built from its model:
//   - "model": "bme280"         a part of qjsx, module qjsx:twin/parts/bme280
//                               (host/twin/parts/bme280/model.js);
//   - "model": "./twin/relay"   a folder of the project: model.js, and
//                               view.js for a view of its own (optional).
// Checked when the twin starts: ids, models, and what each model checks of
// its own fields (pins taken by two parts, an I2C address taken twice). The
// first error stops the twin.
import * as fs from "sys/fs";
import { Board } from "qjsx:twin";

// What an id may be: lower case letters, digits, '-' and '_'.
const ID_PATTERN = /^[a-z0-9_-]+$/;

// The control types the page knows (host/twin/ui/src/partview.ts).
const CONTROL_TYPES = ["slider", "knob", "switch", "select", "button", "value", "led"];

/**
 * True when 'model' names a folder of the project rather than a part of qjsx.
 */
function isProjectModel(model) {
    return model.startsWith("./") || model.startsWith("../") || model.startsWith("/") || /^[A-Za-z]:/.test(model);
}

/**
 * The folder of a project model, absolute.
 */
function modelFolder(projectDir, model) {
    const absolute = model.startsWith("/") || /^[A-Za-z]:/.test(model);
    return absolute ? model : `${projectDir}/${model}`;
}

/**
 * The wiring of the twin: the board's parts ("board" as where they come
 * from), then the project's. Throws on a part that is not an object with an
 * id and a model.
 */
function wiring(boardParts, projectParts) {
    const entries = [
        ...(boardParts ?? []).map(config => ({ config, from: "the board" })),
        ...(projectParts ?? []).map(config => ({ config, from: "qjsx.json" })),
    ];

    // each entry: an object, an id, a model
    const ids = new Set();
    for (const { config, from } of entries) {
        if (typeof config !== "object" || config === null || Array.isArray(config)) {
            throw new Error(`${from}: each part must be an object`);
        }
        if (typeof config.id !== "string" || !ID_PATTERN.test(config.id)) {
            throw new Error(`${from}: part ${JSON.stringify(config.id)}: "id" must be lower case letters, digits, '-' or '_'`);
        }
        if (ids.has(config.id)) {
            throw new Error(`${from}: part "${config.id}": another part has this id`);
        }
        if (typeof config.model !== "string" || config.model === "") {
            throw new Error(`${from}: part "${config.id}": "model" is required`);
        }
        ids.add(config.id);
    }
    return entries;
}

/**
 * The class of a model (the default export of its module).
 */
async function modelClass(config, projectDir) {
    // the module: a part of qjsx, or a folder of the project
    let module;
    if (isProjectModel(config.model)) {
        if (projectDir === undefined) {
            throw new Error(`the model "${config.model}" is a folder of a project, but the twin has none`);
        }
        const file = `${modelFolder(projectDir, config.model)}/model.js`;
        if (!fs.existsSync(file)) {
            throw new Error(`no model: ${file} does not exist`);
        }
        module = await import(file);
    } else {
        try {
            module = await import(`qjsx:twin/parts/${config.model}`);
        } catch {
            throw new Error(`unknown model "${config.model}"`);
        }
    }

    // its class
    if (typeof module.default !== "function") {
        throw new Error(`the model "${config.model}" exports no default class`);
    }
    return module.default;
}

/**
 * The controls a model declares, checked.
 */
function controlsOf(Model, config) {
    const controls = Model.controls ?? [];
    for (const control of controls) {
        if (typeof control?.name !== "string" || !CONTROL_TYPES.includes(control.type)) {
            throw new Error(`the model "${config.model}" declares a control without a name or of an unknown type`);
        }
    }
    return controls;
}

/**
 * The view of a part for the page: a view of its own in the project
 * (view.js), else none (the page uses its controls, or shows its state).
 */
function viewOf(config, projectDir) {
    if (!isProjectModel(config.model) || projectDir === undefined) {
        return null;
    }
    const file = `${modelFolder(projectDir, config.model)}/view.js`;
    return fs.existsSync(file) ? { url: `/parts/${config.id}/view.js`, file } : null;
}

/**
 * Builds every part. 'hooks': { published(part, state), logged(part, text),
 * failed(id, error) } (see Board). Returns { board, parts: [{ part,
 * description, viewFile }] }, description being what the page gets
 * (twin.parts). Throws "part "<id>" (<model>): ..." on the first error.
 */
export async function loadParts({ boardParts, projectParts, projectDir, hooks }) {
    const board = new Board(hooks);
    const parts = [];

    for (const { config } of wiring(boardParts, projectParts)) {
        // the model, then the part (the pins it takes are its own)
        try {
            const Model = await modelClass(config, projectDir);
            const controls = controlsOf(Model, config);
            board._currentPart = config.id;
            const part = new Model(config, board);
            board._currentPart = undefined;

            const view = viewOf(config, projectDir);
            const description = { id: config.id, label: part.label, model: config.model, controls, view: view?.url ?? null };
            parts.push({ part, description, viewFile: view?.file });
        } catch (error) {
            throw new Error(`part "${config.id}" (${config.model}): ${error.message}`);
        }
    }

    // the pins each part took, for the page
    const pinsOfParts = board.pinsOfParts();
    for (const { description } of parts) {
        description.pins = pinsOfParts[description.id] ?? [];
    }
    return { board, parts };
}
