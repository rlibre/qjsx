/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// What 'qjsx build' does with the outlines of the modules of a package
// (compileModule().outline, engine change W):
//   - checkImports(): every name a module imports from another module of the
//     package must be exported by it (else the device would refuse the
//     program when it starts);
//   - pruneModules(): the top-level declarations that nothing uses are left
//     out. On a device every function costs RAM when its module is loaded,
//     even when it never runs (about 275 bytes on a 32-bit board).
//
// What is used, from the code of the program:
//   1. every statement of a module that does something (not a removable
//      declaration, see the outline) is kept, with the names it uses;
//   2. a name used is the declaration that declares it (kept in turn, with
//      the names it uses), or an import: then the export of the other module
//      (followed through re-exports); 'import * as ns' uses every export of
//      the module, however ns is used;
//   3. what remains of the removable declarations is unused.
// A module with a direct eval(), or without outline, keeps everything. A
// top-level declaration is only reachable by its name, written in the code
// (modules do not put their names on globalThis): an object holding a
// function names it, so x["name"]() never reaches a declaration left out.
//
// Leaving out: the text of the declaration (and of a name of an export
// clause nothing imports) is replaced by spaces, line ends kept, and the
// module is compiled again: the positions of everything else are unchanged,
// the debugger reads the original source.
import { normalizeImport } from "qjsx:compile";
import { isNativeModule } from "qjsx:library";
import { placeOf } from "qjsx:util";

const encoder = new TextEncoder();
const decoder = new TextDecoder();

// ### Exports ##################################################################

/**
 * The exports of a module by exported name, and its 'export * from'
 * modules (normalized names).
 */
function exportTable(name, outline) {
    const byName = new Map();
    const starModules = [];

    // an entry without exported name is an 'export * from'
    for (const entry of outline.exports) {
        if (entry.exported === null) {
            starModules.push(normalizeImport(name, entry.module));
        } else {
            byName.set(entry.exported, entry);
        }
    }

    return { byName, starModules };
}

/**
 * True if the module 'name' of 'modules' exports 'exported', itself or
 * through an 'export * from' ('seen' stops the cycles).
 */
function exportsName(modules, name, exported, seen = new Set()) {
    // not a module of the package, or already seen
    const module = modules.get(name);
    if (module === undefined || seen.has(name)) {
        return false;
    }

    seen.add(name);

    // its own exports
    const table = exportTable(name, module.outline);
    if (table.byName.has(exported)) {
        return true;
    }

    // 'export * from' never re-exports 'default'
    if (exported === "default") {
        return false;
    }

    // else the modules it re-exports
    return table.starModules.some(star => exportsName(modules, star, exported, seen));
}

// ### Import check #############################################################

/**
 * Checks that every name imported by a module of 'modules' (Map(name ->
 * { script, source, outline })) from another one is exported by it. Native
 * modules are not checked (the device checks them). Throws the first error,
 * with its position: "src/main.js:3:10: 'sleep' is not exported by
 * stdlib/tasks".
 */
export function checkImports(modules) {
    for (const [name, module] of modules) {
        // a module without outline is not checked
        const hasNoOutline = module.outline === undefined || module.outline === null;
        if (hasNoOutline) {
            continue;
        }

        const sourceBytes = encoder.encode(module.source);

        // the imports, then the re-exports of single names
        const imported = module.outline.imports
            .filter(entry => entry.imported !== "*")
            .map(entry => ({ module: entry.module, name: entry.imported, position: entry.position }));
        const reexported = module.outline.exports
            .filter(entry => entry.exported !== null && entry.module !== null && entry.local !== "*")
            .map(entry => ({ module: entry.module, name: entry.local, position: entry.start }));

        for (const wanted of [...imported, ...reexported]) {
            // only the modules of the package
            const target = normalizeImport(name, wanted.module);
            const isOutside = isNativeModule(target) || !modules.has(target);
            if (isOutside) {
                continue;
            }

            // the name must be exported there
            if (!exportsName(modules, target, wanted.name)) {
                const place = placeOf(module.script.path, sourceBytes, wanted.position);
                throw new Error(`${place}: '${wanted.name}' is not exported by ${target}`);
            }
        }
    }
}

// ### Use of the names #########################################################

/**
 * The state of one module during the walk.
 */
class ModuleUse {
    /**
     * The use of the module 'name' ({ imports, outline }): nothing used yet.
     */
    constructor(name, module) {
        this.name = name;
        this.imports = module.imports;   // [{ specifier, name }]
        this.outline = module.outline ?? null;
        this.hasOutline = this.outline !== null;
        this.keepsAll = !this.hasOutline || this.outline.hasEval;
        this.table = this.hasOutline ? exportTable(name, this.outline) : null;

        // which statement declares each top-level name, which import each
        // local name comes from
        this.declaredBy = new Map();
        this.importOf = new Map();

        if (this.hasOutline) {
            this.outline.statements.forEach((statement, index) => {
                for (const declared of statement.declared) {
                    this.declaredBy.set(declared, index);
                }
            });

            for (const entry of this.outline.imports) {
                this.importOf.set(entry.local, entry);
            }
        }

        // what the walk found used
        this.keptStatements = new Set();   // indexes
        this.usedNames = new Set();
        this.usedExports = new Set();
        this.allExportsUsed = false;
    }
}

/**
 * The walk over the modules of a package (see the top of the file).
 */
class UseWalk {
    /**
     * One ModuleUse per module of 'modules' (Map(name -> module)).
     */
    constructor(modules) {
        this.uses = new Map();

        for (const [name, module] of modules) {
            this.uses.set(name, new ModuleUse(name, module));
        }
    }

    /**
     * 1. every statement that does something is kept. A module that keeps
     * everything (a direct eval) keeps all its statements and exports all its
     * names; one without outline uses every export of what it imports.
     */
    start() {
        for (const use of this.uses.values()) {
            // no outline: every export of what it imports is used
            if (!use.hasOutline) {
                for (const imported of use.imports) {
                    this.useAllExports(imported.name);
                }
                continue;
            }

            // the statements that do something, all of them when it keeps all
            use.outline.statements.forEach((statement, index) => {
                if (!statement.removable || use.keepsAll) {
                    this.keepStatement(use, index);
                }
            });

            // keeping all: every export and every import used
            if (use.keepsAll) {
                this.useAllExports(use.name);
                for (const imported of use.outline.imports) {
                    this.useName(use, imported.local);
                }
            }
        }
    }

    /**
     * A statement is kept: the names it uses are used.
     */
    keepStatement(use, index) {
        if (use.keptStatements.has(index)) {
            return;
        }

        use.keptStatements.add(index);

        for (const name of use.outline.statements[index].references) {
            this.useName(use, name);
        }
    }

    /**
     * 2. A name of a module is used: its declaration, or the export it
     * imports.
     */
    useName(use, name) {
        if (use.usedNames.has(name)) {
            return;
        }

        use.usedNames.add(name);

        // declared in the module: its declaration is kept
        const declaration = use.declaredBy.get(name);
        if (declaration !== undefined) {
            this.keepStatement(use, declaration);
        }

        // imported: the export it names (all of them for 'import * as')
        const imported = use.importOf.get(name);
        if (imported !== undefined) {
            const target = normalizeImport(use.name, imported.module);
            if (imported.imported === "*") {
                this.useAllExports(target);
            } else {
                this.useExport(target, imported.imported);
            }
        }
    }

    /**
     * An export of the module 'name' is used: the name it exports, or the
     * export it re-exports.
     */
    useExport(name, exported) {
        const use = this.uses.get(name);
        if (use === undefined || !use.hasOutline || use.usedExports.has(exported)) {
            return;   // a native module, one without outline (keeps all), or done
        }

        use.usedExports.add(exported);

        // its own export entry, if it has one
        const entry = use.table.byName.get(exported);
        if (entry === undefined) {
            // not its own: one of its 'export * from' modules has it
            for (const star of use.table.starModules) {
                this.useExport(star, exported);
            }
            return;
        }

        this.useEntry(use, entry);
    }

    /**
     * An export entry of a module is used.
     */
    useEntry(use, entry) {
        // a local name exported
        if (entry.module === null) {
            this.useName(use, entry.local);
            return;
        }

        // a re-export: the export of the other module, or all of them
        const target = normalizeImport(use.name, entry.module);
        if (entry.local === "*") {
            this.useAllExports(target);
        } else {
            this.useExport(target, entry.local);
        }
    }

    /**
     * Every export of the module 'name' is used ('import * as').
     */
    useAllExports(name) {
        const use = this.uses.get(name);
        if (use === undefined || !use.hasOutline || use.allExportsUsed) {
            return;
        }

        use.allExportsUsed = true;

        // its own exports
        for (const entry of use.table.byName.values()) {
            use.usedExports.add(entry.exported);
            this.useEntry(use, entry);
        }

        // and those of the modules it re-exports
        for (const star of use.table.starModules) {
            this.useAllExports(star);
        }
    }
}

// ### Leaving out ##############################################################

/**
 * The ranges [start, end) of a module to replace by spaces: its removable
 * statements not kept, and the names of its export clauses nothing imports.
 * Each range comes with what it removes, for the report.
 */
function unusedRanges(use) {
    const ranges = [];

    // the removable statements not kept
    use.outline.statements.forEach((statement, index) => {
        const isUnused = statement.removable && !use.keptStatements.has(index);
        if (isUnused) {
            ranges.push({ start: statement.start, end: statement.end, names: statement.declared });
        }
    });

    // the names of export clauses nothing imports
    for (const entry of use.outline.exports) {
        const isClauseName = entry.end > entry.start;
        if (isClauseName && !use.usedExports.has(entry.exported)) {
            ranges.push({ start: entry.start, end: entry.end, names: [] });
        }
    }

    return ranges;
}

/**
 * The source with the ranges replaced by spaces; line ends stay, so the
 * lines, and the byte offsets, of everything else are unchanged.
 */
function blankRanges(source, ranges) {
    const bytes = encoder.encode(source);
    const SPACE = 0x20;
    const LINE_FEED = 0x0a;
    const CARRIAGE_RETURN = 0x0d;

    // spaces over the bytes of each range, but its line ends
    for (const range of ranges) {
        for (let i = range.start; i < range.end; i++) {
            const isLineEnd = bytes[i] === LINE_FEED || bytes[i] === CARRIAGE_RETURN;
            if (!isLineEnd) {
                bytes[i] = SPACE;
            }
        }
    }

    return decoder.decode(bytes);
}

/**
 * Leaves out of the modules of a package (Map(name -> { script, source,
 * outline })) the top-level declarations nothing uses. Returns one record per
 * module that changes: { name, source (with the declarations replaced by
 * spaces, to compile again), removed (the names left out, "default" for an
 * 'export default') }.
 */
export function pruneModules(modules) {
    // find what is used, from the statements that do something
    const walk = new UseWalk(modules);
    walk.start();

    const changes = [];

    for (const [name, module] of modules) {
        // a module that keeps all does not change
        const use = walk.uses.get(name);
        if (use.keepsAll) {
            continue;
        }

        // nor one without unused range
        const ranges = unusedRanges(use);
        if (ranges.length === 0) {
            continue;
        }

        // the names left out ("*default*" is the local name of 'export default')
        const removed = ranges.flatMap(range => range.names)
            .map(removedName => removedName === "*default*" ? "default" : removedName);
        changes.push({ name, source: blankRanges(module.source, ranges), removed });
    }

    return changes;
}
