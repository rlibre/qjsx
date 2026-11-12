/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "sys/fs": synchronous file system functions, named and behaving like
// their Node counterparts (subset).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "jsbind.h"
#include "memory.h"
#include "files.h"
#include "modules.h"
#include "permissions.h"

namespace {

// A path given by the program, as fs must open it (rt::resolveProgramPath:
// relative to the run's working folder), checked against the run's
// permissions (rt::Permissions::mayAccess). Freed when it goes out of scope;
// error messages keep showing the path the program gave, like Node.
class ProgramPath {
public:
    /**
     * Resolves 'path' and checks that the run may access it this way;
     * valid() is false on a device without file system (errno ENOSYS) or if
     * the access is not granted (errno EACCES).
     */
    ProgramPath(const char *path, rt::Access access)
        : _given(path), _access(access), _resolved(rt::resolveProgramPath(path)), _refused(false) {
        // the tool's own commands have no permissions: everything allowed
        const rt::Permissions *permissions = rt::permissions();
        if (_resolved == nullptr || permissions == nullptr) {
            return;
        }

        // a refused access: no path to open
        if (!permissions->mayAccess(_resolved, access)) {
            memory::release(_resolved);
            _resolved = nullptr;
            _refused = true;
            errno = EACCES;
        }
    }

    /**
     * Frees the resolved path.
     */
    ~ProgramPath() {
        memory::release(_resolved);
    }

    /**
     * False on a device without file system, or if the access is refused.
     */
    bool valid() const {
        return _resolved != nullptr;
    }

    /**
     * True if the access was refused by the permissions.
     */
    bool refused() const {
        return _refused;
    }

    /**
     * The path to give to files::.
     */
    const char *text() const {
        return _resolved;
    }

    /**
     * The JS error of an invalid path: EACCES with how to grant the access
     * when refused, else the system error in errno, naming the given path.
     */
    js::Failure fail() const {
        // not refused by the permissions: the system's error
        if (!_refused) {
            return js::failSystem(errno, _given);
        }

        // refused: say which access, to what, and how to grant it
        char what[512];
        const char *hint = rt::permissions()->hint();
        snprintf(what, sizeof(what), "%s access to \"%s\" is not allowed%s%s", rt::accessName(_access), _given,
                 hint[0] != 0 ? ": " : "", hint);
        return js::failSystem(EACCES, what);
    }

private:
    const char *_given;     // the path the program gave (borrowed: lives as long as the call)
    rt::Access  _access;    // how the path is used
    char       *_resolved;  // malloc'd; nullptr: no file system, or refused
    bool        _refused;   // the permissions refused the access
};

struct MkdirOptions {
    bool recursive = false;  // also create the missing parents; an existing directory is no error
};
JS_STRUCT(MkdirOptions, recursive);

struct RmOptions {
    bool recursive = false;  // also remove a directory and its content
    bool force = false;      // no error when the path does not exist
};
JS_STRUCT(RmOptions, recursive, force);

class Stats {
    JS_CLASS(Stats);

public:
    JS_EXPORT(size) double size = 0;        // bytes
    JS_EXPORT(mtimeMs) double mtimeMs = 0;  // ms since 1970

    /**
     * stats.isFile(): true for a regular file.
     */
    JS_EXPORT(isFile) bool isFile() const {
        return file;
    }

    /**
     * stats.isDirectory(): true for a directory.
     */
    JS_EXPORT(isDirectory) bool isDirectory() const {
        return directory;
    }

    bool file = false;       // regular file (isFile())
    bool directory = false;  // directory (isDirectory())
};

/**
 * True if readFileSync's options ask for text ("utf8", { encoding: "utf8" }...);
 * another encoding name is a TypeError, and an exception thrown while reading
 * the options goes to the caller (check js::failed() after the call).
 */
bool wantsText(js::Optional<js::Value> options) {
    // no options (undefined, null): bytes
    if (!options.has() || options.value().isUndefined() || options.value().isNull()) {
        return false;
    }

    // the encoding is the option itself, or the 'encoding' property of an
    // object (a getter may throw: its exception goes to the caller)
    JSContext *ctx = options.value().context();
    JSValue encoding = JS_IsObject(options.value().get())
        ? JS_GetPropertyStr(ctx, options.value().get(), "encoding")
        : JS_DupValue(ctx, options.value().get());
    if (JS_IsException(encoding)) {
        js::detail::markPendingException();
        return false;
    }

    // no string encoding (an object without one) means bytes
    if (!JS_IsString(encoding)) {
        JS_FreeValue(ctx, encoding);
        return false;
    }

    // read the name; a failed conversion (out of memory) leaves its exception
    // pending, which goes to the caller
    const char *name = JS_ToCString(ctx, encoding);
    JS_FreeValue(ctx, encoding);
    if (name == nullptr) {
        js::detail::markPendingException();
        return false;
    }

    // only "utf8" and "utf-8" are known, any other name is a TypeError
    bool text = strcmp(name, "utf8") == 0 || strcmp(name, "utf-8") == 0;
    if (!text) {
        js::fail<js::TypeError>("fs: unsupported encoding '%s' (only utf8)", name);
    }

    JS_FreeCString(ctx, name);
    return text;
}

/**
 * fs.readFileSync(path, options), like Node's: the content as a string with
 * encoding utf8, otherwise as a Uint8Array (Node returns a Buffer).
 */
js::Value readFileSync(js::Str path, js::Optional<js::Value> options) {
    // check the encoding first, so that a bad option fails before any I/O
    bool text = wantsText(options);
    if (js::failed()) {
        return js::Value();
    }

    // where the file is (relative to the data directory on a device)
    ProgramPath file(path.data(), rt::Access::read);
    if (!file.valid()) {
        return file.fail();
    }

    // read the whole file; system error with a Node-like code on failure
    size_t size = 0;
    uint8_t *data = files::readAll(file.text(), &size);
    if (data == nullptr) {
        return js::failSystem(errno, path.data());
    }

    // copy it into a JS string or array, then free the C buffer
    JSContext *ctx = js::context();
    JSValue result = text ? JS_NewStringLen(ctx, (const char *)data, size)
                          : js::detail::newUint8Array(ctx, data, size);
    memory::release(data);
    return js::Value(ctx, result);
}

/**
 * fs.writeFileSync(path, data), like Node's without options: creates or
 * replaces the file with a string (written as UTF-8) or bytes.
 */
void writeFileSync(js::Str path, js::Value data) {
    JSContext *ctx = js::context();

    // where the file goes (relative to the data directory on a device)
    ProgramPath file(path.data(), rt::Access::write);
    if (!file.valid()) {
        return (void)file.fail();
    }

    // a string: convert it to UTF-8; a failed conversion (out of memory)
    // leaves its JS exception pending, which goes to the caller
    if (JS_IsString(data.get())) {
        size_t size = 0;
        const char *text = JS_ToCStringLen(ctx, &size, data.get());
        if (text == nullptr) {
            js::detail::markPendingException();
            return;
        }

        // write it; system error with a Node-like code on failure (errno
        // kept first: freeing the text may change it)
        bool ok = files::writeAll(file.text(), text, size);
        int error = errno;
        JS_FreeCString(ctx, text);
        if (!ok) {
            return (void)js::failSystem(error, path.data());
        }

        return;
    }

    // bytes (typed array or ArrayBuffer): a view on the JS memory, kept alive
    // by the reference in 'buffer'; anything else is a TypeError
    uint8_t *bytes = nullptr;
    size_t size = 0;
    JSValue buffer = JS_UNDEFINED;
    if (!js::detail::bytesOf(ctx, data.get(), &bytes, &size, &buffer)) {
        return (void)js::fail<js::TypeError>("writeFileSync: data must be a string or a Uint8Array");
    }

    // write them, then release the reference
    bool ok = files::writeAll(file.text(), bytes, size);
    JS_FreeValue(ctx, buffer);
    if (!ok) {
        return (void)js::failSystem(errno, path.data());
    }
}

/**
 * fs.existsSync(path), like Node's: true if the path exists (file,
 * directory...).
 */
bool existsSync(js::Str path) {
    // nothing exists on a device without file system
    ProgramPath file(path.data(), rt::Access::read);
    if (file.refused()) {
        return file.fail();
    }

    if (!file.valid()) {
        return false;
    }

    files::Status status;
    return files::stat(file.text(), &status) && status.exists;
}

/**
 * fs.statSync(path), like Node's (subset): a Stats object with size, mtimeMs,
 * isFile() and isDirectory().
 */
js::Value statSync(js::Str path) {
    // stat the path; system error if it does not exist (or no file system)
    ProgramPath file(path.data(), rt::Access::read);
    if (!file.valid()) {
        return file.fail();
    }

    files::Status status;
    if (!files::stat(file.text(), &status)) {
        return js::failSystem(errno, path.data());
    }

    // create the JS Stats object and fill it
    js::Created<Stats> stats = js::create<Stats>();
    if (stats.object == nullptr) {
        return js::Value();
    }

    stats.object->size = (double)status.size;
    stats.object->mtimeMs = status.modifiedMs;
    stats.object->file = status.isFile;
    stats.object->directory = status.isDirectory;
    return stats.value;
}

struct ArrayFiller {
    JSContext *ctx;  // borrowed
    JSValue array;   // the array being filled (freed by readdirSync on failure)
    uint32_t count;  // names appended so far (next index)
};

/**
 * Directory visitor of readdirSync: appends the name to the ArrayFiller's
 * array.
 */
bool addName(void *opaque, const char *name) {
    ArrayFiller *filler = (ArrayFiller *)opaque;
    JS_SetPropertyUint32(filler->ctx, filler->array, filler->count++, JS_NewString(filler->ctx, name));
    return true;
}

/**
 * fs.readdirSync(path), like Node's: the entry names of the directory, in the
 * file system's order (Node sorts on some platforms).
 */
js::Value readdirSync(js::Str path) {
    // where the directory is (relative to the data directory on a device)
    ProgramPath directory(path.data(), rt::Access::read);
    if (!directory.valid()) {
        return directory.fail();
    }

    // fill a new array from the listing; on failure free it and throw a
    // system error
    JSContext *ctx = js::context();
    ArrayFiller filler = { ctx, JS_NewArray(ctx), 0 };
    if (!files::listDirectory(directory.text(), &addName, &filler)) {
        JS_FreeValue(ctx, filler.array);
        return js::failSystem(errno, path.data());
    }

    return js::Value(ctx, filler.array);
}

/**
 * fs.mkdirSync(path, { recursive }), like Node's: creates the directory; with
 * recursive, also its missing parents, and an existing directory is no error.
 */
void mkdirSync(js::Str path, js::Optional<MkdirOptions> options) {
    // where it goes (relative to the data directory on a device)
    ProgramPath directory(path.data(), rt::Access::write);
    if (!directory.valid()) {
        return (void)directory.fail();
    }

    // create it, with its parents if recursive
    bool recursive = options.has() && options.value().recursive;
    bool ok = recursive ? files::makeDirectories(directory.text()) : files::makeDirectory(directory.text());
    if (!ok) {
        return (void)js::failSystem(errno, path.data());
    }
}

/**
 * Deletes a file or a whole directory tree (defined below; removeChild and
 * removeTree call each other).
 */
bool removeTree(const char *path);

struct TreeRemover {
    const char *directory;  // path of the directory being emptied; borrowed
    bool ok;                // false once a removal failed
};

/**
 * Directory visitor of removeTree: removes "directory/name" and everything in
 * it; returns false, which stops the listing, on failure.
 */
bool removeChild(void *opaque, const char *name) {
    // build the path "directory/name"
    TreeRemover *remover = (TreeRemover *)opaque;
    size_t size = strlen(remover->directory) + strlen(name) + 2;
    char *child = (char *)memory::allocate(size);
    snprintf(child, size, "%s/%s", remover->directory, name);

    // remove it, keeping the result for removeTree
    remover->ok = removeTree(child);
    memory::release(child);
    return remover->ok;
}

/**
 * Deletes a file, or a directory with everything in it. Stops at the first
 * failure, errno set; what was already deleted stays deleted.
 */
bool removeTree(const char *path) {
    // describe the entry itself: a link is never followed (lstat), else the
    // contents of its target, outside the tree, would be deleted
    files::Status status;
    if (!files::linkStatus(path, &status)) {
        return false;
    }

    // not a directory (a POSIX link included): delete it directly
    if (!status.isDirectory) {
        return files::removeFile(path);
    }

    // a Windows directory link or junction: remove the link only
    if (status.isLink) {
        return files::removeDirectory(path);
    }

    // a directory: first remove each of its entries, depth first
    TreeRemover remover = { path, true };
    if (!files::listDirectory(path, &removeChild, &remover) || !remover.ok) {
        return false;
    }

    // then the directory itself, now empty
    return files::removeDirectory(path);
}

/**
 * fs.rmSync(path, { recursive, force }), like Node's: deletes a file, or a
 * directory tree with recursive.
 */
void rmSync(js::Str path, js::Optional<RmOptions> options) {
    bool recursive = options.has() && options.value().recursive;
    bool force = options.has() && options.value().force;

    // where it is (relative to the data directory on a device)
    ProgramPath target(path.data(), rt::Access::write);
    if (!target.valid()) {
        return (void)target.fail();
    }

    // stat the path: a missing path is ignored with force, an error otherwise
    files::Status status;
    if (!files::stat(target.text(), &status)) {
        if (force && errno == ENOENT) {
            return;
        }

        return (void)js::failSystem(errno, path.data());
    }

    // a directory needs recursive: EISDIR (Node's code is ERR_FS_EISDIR)
    if (status.isDirectory && !recursive) {
        return (void)js::failSystem(EISDIR, path.data());
    }

    // delete the file or the whole tree
    bool ok = status.isDirectory ? removeTree(target.text()) : files::removeFile(target.text());
    if (!ok) {
        return (void)js::failSystem(errno, path.data());
    }
}

/**
 * fs.renameSync(from, to), like Node's: renames or moves, replacing 'to' if
 * it exists.
 */
void renameSync(js::Str from, js::Str to) {
    // both paths relative to the data directory on a device
    ProgramPath source(from.data(), rt::Access::write);
    if (!source.valid()) {
        return (void)source.fail();
    }

    ProgramPath destination(to.data(), rt::Access::write);
    if (!destination.valid()) {
        return (void)destination.fail();
    }

    // rename; the error names the source, like Node
    if (!files::rename(source.text(), destination.text())) {
        return (void)js::failSystem(errno, from.data());
    }
}

/**
 * fs.unlinkSync(path), like Node's: deletes a file.
 */
void unlinkSync(js::Str path) {
    // where it is (relative to the data directory on a device)
    ProgramPath file(path.data(), rt::Access::write);
    if (!file.valid()) {
        return (void)file.fail();
    }

    // delete it
    if (!files::removeFile(file.text())) {
        return (void)js::failSystem(errno, path.data());
    }
}

} // namespace

/**
 * The "sys/fs" module: its functions and the Stats class.
 */
JS_MODULE(fs, module) {
    JS_FUNCTION(module, readFileSync);
    JS_FUNCTION(module, writeFileSync);
    JS_FUNCTION(module, existsSync);
    JS_FUNCTION(module, statSync);
    JS_FUNCTION(module, readdirSync);
    JS_FUNCTION(module, mkdirSync);
    JS_FUNCTION(module, rmSync);
    JS_FUNCTION(module, renameSync);
    JS_FUNCTION(module, unlinkSync);
    JS_ADD_CLASS(module, Stats);
}
