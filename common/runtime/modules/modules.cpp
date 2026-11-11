/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

#include "modules.h"

#include <string.h>

/**
 * Creates the "fs" module (defined by JS_MODULE in fs.cpp).
 */
JSModuleDef *jsNewModule_fs(JSContext *ctx, const char *moduleName);

/**
 * Creates the "crypto" module (defined by JS_MODULE in crypto.cpp).
 */
JSModuleDef *jsNewModule_crypto(JSContext *ctx, const char *moduleName);

namespace rt {

namespace {

NativeModule nativeModules[32] = {
    { "fs", &jsNewModule_fs },
    { "crypto", &jsNewModule_crypto },
};
// the modules of one platform only (net, serial, compiler...) are added by its
// main with registerNativeModule()
int nativeModuleCount = 2;

// Filled before main() by EmbeddedRegistration objects: initialized on first
// use, because the order of static initializations between files is unknown.
// Only the desktop tool registers modules (its commands and its own copy of
// the standard library): a device firmware embeds no JS, its table is empty.
struct EmbeddedTable {
    EmbeddedModule modules[64];  // in registration order
    int count;                   // entries used in modules
};

/**
 * The table of embedded modules, created empty on first use.
 */
EmbeddedTable &embeddedTable() {
    // a function-local static: EmbeddedRegistration objects of other files
    // can use it before main(), whatever the initialization order of the files
    static EmbeddedTable table = { {}, 0 };
    return table;
}

} // namespace

/**
 * Native module registered under this name, or nullptr.
 */
const NativeModule *findNativeModule(const char *name) {
    // linear search: the table is small; the first module with the name wins
    for (int i = 0; i < nativeModuleCount; i++) {
        if (strcmp(nativeModules[i].name, name) == 0) {
            return &nativeModules[i];
        }
    }

    return nullptr;
}

/**
 * Embedded JS module registered under this name, or nullptr.
 */
const EmbeddedModule *findEmbeddedModule(const char *name) {
    EmbeddedTable &table = embeddedTable();

    // linear search: the table is small; the first module with the name wins
    for (int i = 0; i < table.count; i++) {
        if (strcmp(table.modules[i].name, name) == 0) {
            return &table.modules[i];
        }
    }

    return nullptr;
}

/**
 * Appends a module to the embedded table; the name and source are kept, not
 * copied.
 */
void registerEmbeddedModule(const char *name, const char *source) {
    // silently ignored when the table is full (64 entries); a name already
    // present is not replaced, since the search finds the first one
    EmbeddedTable &table = embeddedTable();
    int capacity = (int)(sizeof(table.modules) / sizeof(table.modules[0]));
    if (table.count < capacity) {
        table.modules[table.count++] = EmbeddedModule{ name, source };
    }
}

/**
 * Appends a module to the native table; the name is kept, not copied.
 */
void registerNativeModule(const char *name, NativeModuleFactory *create) {
    // silently ignored when the table is full (32 entries); a name already
    // present is not replaced, since the search finds the first one
    int capacity = (int)(sizeof(nativeModules) / sizeof(nativeModules[0]));
    if (nativeModuleCount < capacity) {
        nativeModules[nativeModuleCount++] = NativeModule{ name, create };
    }
}

} // namespace rt
