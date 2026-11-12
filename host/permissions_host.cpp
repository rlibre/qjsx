/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The permissions of a program of the PC (see permissions_host.h).
#include "permissions_host.h"
#include "files.h"
#include "memory.h"
#include "package.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "quickjs.h"

namespace host {

namespace {

// The end of a refusal message: how to grant the access.
const char *const SCRIPT_HINT = "grant it with an option of qjsx run (-R / --allow-read, -W / --allow-write, "
                                "-N / --allow-net, -E / --allow-env, --allow-serial, -A)";
const char *const PACKAGE_HINT = "allow it in \"desktop.permissions\" of qjsx.json";

const char *const ROOT_VARIABLE = "QJSX_ROOT_FOLDER";   // the root folder in the environment

// The kinds of permission.
enum class Kind {
    all,     // everything
    read,    // files: read
    write,   // files: write
    net,     // network
    env,     // environment variables
    serial,  // serial ports
};

// An option of qjsx run.
struct RunOption {
    const char *longName;   // "--allow-read"
    const char *shortName;  // "-R", nullptr: none
    Kind        kind;       // what it grants
};

const RunOption RUN_OPTIONS[] = {
    { "--allow-all", "-A", Kind::all },
    { "--allow-read", "-R", Kind::read },
    { "--allow-write", "-W", Kind::write },
    { "--allow-net", "-N", Kind::net },
    { "--allow-env", "-E", Kind::env },
    { "--allow-serial", nullptr, Kind::serial },
};

// The keys of "desktop.permissions" in the manifest.
struct ManifestKind {
    const char *key;   // "read"
    Kind        kind;  // what it grants
};

const ManifestKind MANIFEST_KINDS[] = {
    { "read", Kind::read },
    { "write", Kind::write },
    { "net", Kind::net },
    { "env", Kind::env },
    { "serial", Kind::serial },
};

// The values of the variables of the paths of the manifest (owned strings;
// nullptr: not available).
struct Variables {
    char       *executableFolder;   // ${executableFolder}
    char       *userAppDataFolder;  // ${userAppDataFolder}
    char       *currentFolder;      // ${currentFolder}
    const char *rootFolder;         // ${rootFolder} (borrowed: the working folder)
};

/**
 * Grants one item of a kind (nullptr: the whole kind). False after printing
 * why (a folder that cannot be resolved).
 */
bool grant(rt::Permissions *permissions, Kind kind, const char *item) {
    switch (kind) {
        case Kind::all: {
            permissions->allowEverything();
            return true;
        }
        case Kind::read:
        case Kind::write: {
            // files: a folder, by its real path, or everywhere
            rt::Access access = kind == Kind::read ? rt::Access::read : rt::Access::write;
            if (item == nullptr) {
                permissions->allowFilesEverywhere(access);
                return true;
            }

            if (!permissions->allowFiles(access, item)) {
                fprintf(stderr, "qjsx: cannot grant %s access to %s: %s\n", rt::accessName(access), item,
                        strerror(errno));
                return false;
            }

            return true;
        }
        case Kind::net: {
            permissions->allowNet(item);
            return true;
        }
        case Kind::env: {
            permissions->allowEnv(item);
            return true;
        }
        case Kind::serial: {
            permissions->allowSerial(item);
            return true;
        }
    }

    return false;
}

/**
 * Grants each item of a comma-separated list; an empty list grants the
 * whole kind. False after printing why.
 */
bool grantList(rt::Permissions *permissions, Kind kind, const char *list) {
    // no list: the whole kind
    if (list[0] == 0) {
        return grant(permissions, kind, nullptr);
    }

    // each item between commas (empty items skipped)
    char *copy = memory::copyText(list, strlen(list));
    bool ok = true;
    char *item = copy;
    while (ok && item != nullptr) {
        char *comma = strchr(item, ',');
        if (comma != nullptr) {
            *comma = 0;
        }

        if (item[0] != 0) {
            ok = grant(permissions, kind, item);
        }

        item = comma != nullptr ? comma + 1 : nullptr;
    }

    memory::release(copy);
    return ok;
}

/**
 * If 'word' is 'name' or "name=value": its value ("" without one); else
 * nullptr.
 */
const char *optionValue(const char *word, const char *name) {
    if (name == nullptr) {
        return nullptr;
    }

    size_t length = strlen(name);
    if (strncmp(word, name, length) != 0) {
        return nullptr;
    }

    if (word[length] == 0) {
        return "";
    }

    return word[length] == '=' ? word + length + 1 : nullptr;
}

/**
 * True if the real path 'path' is the root of a file system: "/", "C:/",
 * "//server/share".
 */
bool isFileSystemRoot(const char *path) {
    // POSIX, and a drive
    size_t length = strlen(path);
    bool isSlash = strcmp(path, "/") == 0;
    bool isDrive = (length == 2 || (length == 3 && path[2] == '/')) && path[1] == ':';
    if (isSlash || isDrive) {
        return true;
    }

    // a network share: "//server/share", nothing after it
    if (strncmp(path, "//", 2) != 0) {
        return false;
    }

    const char *server = strchr(path + 2, '/');
    return server == nullptr || strchr(server + 1, '/') == nullptr;
}

/**
 * The real path of a root folder (malloc'd), or nullptr after printing why:
 * unreadable, or the root of a file system.
 */
char *resolveRoot(const char *given) {
    // its real path (it may not exist yet)
    char *real = files::realPath(given);
    if (real == nullptr) {
        fprintf(stderr, "qjsx: cannot use the root folder %s: %s\n", given, strerror(errno));
        return nullptr;
    }

    // a whole disk would be in reach of the program
    if (isFileSystemRoot(real)) {
        fprintf(stderr, "qjsx: the root folder cannot be the root of a file system (%s)\n", real);
        memory::release(real);
        return nullptr;
    }

    return real;
}

/**
 * The root folder given at launch: the option, else the environment
 * variable (malloc'd), else nullptr.
 */
char *launchRoot(const char *rootOption) {
    if (rootOption != nullptr) {
        return memory::copyText(rootOption, strlen(rootOption));
    }

    return files::environmentValue(ROOT_VARIABLE);
}

/**
 * "<base><suffix>" (malloc'd).
 */
char *join(const char *base, const char *suffix) {
    size_t size = strlen(base) + strlen(suffix) + 1;
    char *joined = (char *)memory::allocate(size);
    snprintf(joined, size, "%s%s", base, suffix);
    return joined;
}

/**
 * The folder of the system for the data of the application 'name', for the
 * user running it (malloc'd), or nullptr if the environment does not tell:
 * Windows %APPDATA%/<name>, macOS ~/Library/Application Support/<name>,
 * elsewhere $XDG_DATA_HOME/<name> or ~/.local/share/<name>.
 */
char *userAppDataFolder(const char *name) {
    // the base folder of the system
#if defined(_WIN32)
    char *base = files::environmentValue("APPDATA");
#elif defined(__APPLE__)
    char *home = files::environmentValue("HOME");
    char *base = home != nullptr ? join(home, "/Library/Application Support") : nullptr;
    memory::release(home);
#else
    char *base = files::environmentValue("XDG_DATA_HOME");
    if (base == nullptr || !files::isAbsolute(base)) {
        memory::release(base);
        char *home = files::environmentValue("HOME");
        base = home != nullptr ? join(home, "/.local/share") : nullptr;
        memory::release(home);
    }
#endif
    if (base == nullptr) {
        return nullptr;
    }

    // <base>/<name>, '/' separators
    size_t size = strlen(base) + 1 + strlen(name) + 1;
    char *folder = (char *)memory::allocate(size);
    snprintf(folder, size, "%s/%s", base, name);
    memory::release(base);
    files::useSlashes(folder);
    return folder;
}

/**
 * The path 'text' of the manifest with its variable replaced (malloc'd), or
 * nullptr after printing why ('where' names it in the message). A path
 * starts with ${variable} or is absolute.
 */
char *expand(const char *text, const Variables *variables, const char *where) {
    // an absolute path: as it is
    if (strncmp(text, "${", 2) != 0) {
        if (files::isAbsolute(text)) {
            return memory::copyText(text, strlen(text));
        }

        fprintf(stderr, "qjsx: %s: \"%s\" must start with ${executableFolder}, ${userAppDataFolder}, "
                        "${currentFolder} or ${rootFolder}, or be absolute\n", where, text);
        return nullptr;
    }

    // the variable, followed by nothing or by '/'
    const char *close = strchr(text, '}');
    const char *rest = close != nullptr ? close + 1 : nullptr;
    if (rest == nullptr || (rest[0] != 0 && rest[0] != '/')) {
        fprintf(stderr, "qjsx: %s: \"%s\": a variable is followed by '/' or ends the path\n", where, text);
        return nullptr;
    }

    // its value
    size_t nameLength = (size_t)(close - text) - 2;
    const char *name = text + 2;
    const char *value = nullptr;
    bool known = true;
    if (nameLength == 16 && strncmp(name, "executableFolder", 16) == 0) {
        value = variables->executableFolder;
    } else if (nameLength == 17 && strncmp(name, "userAppDataFolder", 17) == 0) {
        value = variables->userAppDataFolder;
    } else if (nameLength == 13 && strncmp(name, "currentFolder", 13) == 0) {
        value = variables->currentFolder;
    } else if (nameLength == 10 && strncmp(name, "rootFolder", 10) == 0) {
        value = variables->rootFolder;
    } else {
        known = false;
    }

    if (!known || value == nullptr) {
        const char *reason = known ? "has no value here" : "is unknown";
        fprintf(stderr, "qjsx: %s: the variable in \"%s\" %s\n", where, text, reason);
        return nullptr;
    }

    return join(value, rest);
}

/**
 * A string property of 'object' (malloc'd), or nullptr if missing or not a
 * string.
 */
char *stringProperty(JSContext *ctx, JSValueConst object, const char *name) {
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    const char *text = JS_IsString(value) ? JS_ToCString(ctx, value) : nullptr;
    char *copy = text != nullptr ? memory::copyText(text, strlen(text)) : nullptr;
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, value);
    return copy;
}

/**
 * Grants one kind of "desktop.permissions": true (everything) or a list of
 * strings (paths expanded for files). False after printing why.
 */
bool grantManifestKind(JSContext *ctx, JSValueConst value, const ManifestKind *kind, const Variables *variables,
                       rt::Permissions *permissions) {
    // true: everything; false: nothing
    if (JS_IsBool(value)) {
        return JS_ToBool(ctx, value) == 0 || grant(permissions, kind->kind, nullptr);
    }

    // else a list
    char where[64];
    snprintf(where, sizeof(where), "desktop.permissions.%s", kind->key);
    if (!JS_IsArray(ctx, value)) {
        fprintf(stderr, "qjsx: %s must be true or a list of strings\n", where);
        return false;
    }

    // each item: a path to expand (files) or a name
    bool isFiles = kind->kind == Kind::read || kind->kind == Kind::write;
    bool ok = true;
    for (uint32_t i = 0; ok; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, value, i);
        if (JS_IsUndefined(item)) {
            break;
        }

        const char *text = JS_IsString(item) ? JS_ToCString(ctx, item) : nullptr;
        JS_FreeValue(ctx, item);
        if (text == nullptr) {
            fprintf(stderr, "qjsx: %s must hold strings only\n", where);
            return false;
        }

        char *expanded = isFiles ? expand(text, variables, where) : memory::copyText(text, strlen(text));
        JS_FreeCString(ctx, text);
        ok = expanded != nullptr && grant(permissions, kind->kind, expanded);
        memory::release(expanded);
    }

    return ok;
}

/**
 * Grants "desktop.permissions" (an object of kinds). False after printing
 * why.
 */
bool grantManifest(JSContext *ctx, JSValueConst granted, const Variables *variables, rt::Permissions *permissions) {
    if (!JS_IsObject(granted)) {
        fprintf(stderr, "qjsx: desktop.permissions must be an object\n");
        return false;
    }

    // each kind present
    bool ok = true;
    for (const ManifestKind &kind : MANIFEST_KINDS) {
        JSValue value = JS_GetPropertyStr(ctx, granted, kind.key);
        if (!JS_IsUndefined(value)) {
            ok = ok && grantManifestKind(ctx, value, &kind, variables, permissions);
        }

        JS_FreeValue(ctx, value);
    }

    return ok;
}

/**
 * The root folder of a package run (malloc'd real path; *root nullptr when
 * there is none): the launch's, else "desktop.rootFolder" expanded. False
 * after printing why.
 */
bool packageRoot(JSContext *ctx, JSValueConst desktop, const Variables *variables, const char *rootOption,
                 char **root) {
    // given at launch
    *root = nullptr;
    char *given = launchRoot(rootOption);

    // else declared by the program (no ${rootFolder} in it, of course)
    if (given == nullptr) {
        char *declared = stringProperty(ctx, desktop, "rootFolder");
        if (declared == nullptr) {
            return true;
        }

        given = expand(declared, variables, "desktop.rootFolder");
        memory::release(declared);
        if (given == nullptr) {
            return false;
        }
    }

    // its real path, not a whole disk
    *root = resolveRoot(given);
    memory::release(given);
    return *root != nullptr;
}

/**
 * Reads the manifest of the package into a context: the parsed object, or
 * JS_EXCEPTION after printing why.
 */
JSValue parseManifest(JSContext *ctx, const uint8_t *package, size_t size) {
    // find manifest.json in the package
    package::Reader reader;
    const package::Entry *entry = reader.open(package, size) ? reader.find("manifest.json") : nullptr;
    if (entry == nullptr) {
        fprintf(stderr, "qjsx: invalid package: no manifest.json\n");
        return JS_EXCEPTION;
    }

    // parse it (zero-terminated copy)
    char *text = memory::copyText((const char *)entry->data, entry->size);
    JSValue manifest = JS_ParseJSON(ctx, text, entry->size, "manifest.json");
    memory::release(text);
    if (!JS_IsObject(manifest)) {
        JS_FreeValue(ctx, manifest);
        fprintf(stderr, "qjsx: invalid package: manifest.json is not a JSON object\n");
        return JS_EXCEPTION;
    }

    return manifest;
}

} // namespace

/**
 * One option of qjsx run.
 */
bool readRunOption(const char *word, rt::Permissions *permissions, const char **rootFolder, bool *known) {
    // the root folder
    size_t rootLength = strlen(ROOT_FOLDER_OPTION);
    if (strncmp(word, ROOT_FOLDER_OPTION, rootLength) == 0) {
        *known = true;
        *rootFolder = word + rootLength;
        if (word[rootLength] == 0) {
            fprintf(stderr, "qjsx: %s needs a folder\n", ROOT_FOLDER_OPTION);
            return false;
        }

        return true;
    }

    // a permission, long or short name
    for (const RunOption &option : RUN_OPTIONS) {
        const char *value = optionValue(word, option.longName);
        if (value == nullptr) {
            value = optionValue(word, option.shortName);
        }

        if (value == nullptr) {
            continue;
        }

        *known = true;
        if (option.kind == Kind::all && value[0] != 0) {
            fprintf(stderr, "qjsx: %s takes no value\n", option.longName);
            return false;
        }

        return grantList(permissions, option.kind, value);
    }

    *known = false;
    return false;
}

/**
 * 1. parse the manifest in a short-lived context;
 * 2. the variables: executable folder, current folder, user app data folder
 *    (from the program's name);
 * 3. the root folder: the program's working folder;
 * 4. the granted kinds; a root without "permissions" may be read.
 */
bool applyDesktopSection(const uint8_t *package, size_t size, const char *programFolder, const char *rootOption,
                         rt::Permissions *permissions) {
    // the manifest, and its "desktop" section ({} without one)
    permissions->setHint(PACKAGE_HINT);
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(runtime);
    JSValue manifest = parseManifest(ctx, package, size);
    JSValue desktop = JS_IsException(manifest) ? JS_UNDEFINED : JS_GetPropertyStr(ctx, manifest, "desktop");
    bool ok = !JS_IsException(manifest);
    if (ok && JS_IsUndefined(desktop)) {
        desktop = JS_NewObject(ctx);
    }

    // the variables
    char *name = ok ? stringProperty(ctx, manifest, "name") : nullptr;
    Variables variables = {
        memory::copyText(programFolder, strlen(programFolder)),
        name != nullptr ? userAppDataFolder(name) : nullptr,
        files::currentDirectory(),
        nullptr,
    };

    // the root folder: the program's process.cwd()
    char *root = nullptr;
    ok = ok && packageRoot(ctx, desktop, &variables, rootOption, &root);
    if (root != nullptr) {
        permissions->setWorkingFolder(root);
        variables.rootFolder = root;
    }

    // the granted kinds; without them, a root may be read
    JSValue granted = ok ? JS_GetPropertyStr(ctx, desktop, "permissions") : JS_UNDEFINED;
    if (ok && !JS_IsUndefined(granted)) {
        ok = grantManifest(ctx, granted, &variables, permissions);
    } else if (ok && root != nullptr) {
        ok = grant(permissions, Kind::read, root);
    }

    // free everything
    JS_FreeValue(ctx, granted);
    JS_FreeValue(ctx, desktop);
    JS_FreeValue(ctx, manifest);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    memory::release(name);
    memory::release(variables.executableFolder);
    memory::release(variables.userAppDataFolder);
    memory::release(variables.currentFolder);
    memory::release(root);
    return ok;
}

/**
 * The root folder of a script run, and the hint of its refusals.
 */
bool applyScriptRoot(const char *rootOption, rt::Permissions *permissions) {
    // the refusals point at the options of qjsx run
    permissions->setHint(SCRIPT_HINT);

    // a root folder given at launch, if any
    char *given = launchRoot(rootOption);
    if (given == nullptr) {
        return true;
    }

    char *root = resolveRoot(given);
    memory::release(given);
    if (root == nullptr) {
        return false;
    }

    permissions->setWorkingFolder(root);
    memory::release(root);
    return true;
}

/**
 * The folder of a file.
 */
char *folderOf(const char *path) {
    // the real path of the file
    char *real = files::realPath(path);
    if (real == nullptr) {
        return nullptr;
    }

    // cut its last component, keeping a root ("/", "C:/")
    char *slash = strrchr(real, '/');
    if (slash == nullptr) {
        return real;
    }

    bool isRootSlash = slash == real || (slash == real + 2 && real[1] == ':');
    slash[isRootSlash ? 1 : 0] = 0;
    return real;
}

} // namespace host
