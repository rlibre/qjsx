/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

#include "agent.h"
#include "memory.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "debugger.h"
#include "jsbind_error.h"
#include "modules.h"
#include "protocol.h"
#include "ram_storage.h"

namespace agent {

/**
 * Payload of a request: the pairs of its map, by integer key.
 * Text and bytes point into the received message: valid while it is handled.
 */
struct Agent::Fields {
    struct Field {
        int64_t key;           // protocol KEY_*
        cbor::Type type;       // type of the value
        int64_t integer;       // value if an integer (Unsigned or Negative)
        const uint8_t *data;   // text or bytes
        size_t size;           // bytes of 'data'
    };
    Field items[16];   // the first 'count' are used
    int count = 0;     // fields read

    /**
     * The field with this key, or nullptr.
     */
    const Field *find(int64_t key) const {
        // linear search: 16 fields at most
        for (int i = 0; i < count; i++) {
            if (items[i].key == key) {
                return &items[i];
            }
        }

        return nullptr;
    }

    /**
     * Integer field. False if missing or of another type.
     */
    bool integer(int64_t key, int64_t *value) const {
        const Field *field = find(key);
        bool isInteger = field != nullptr && (field->type == cbor::Type::Unsigned || field->type == cbor::Type::Negative);
        if (isInteger) {
            *value = field->integer;
        }

        return isInteger;
    }

    /**
     * Byte string field. False if missing or of another type.
     */
    bool bytes(int64_t key, const uint8_t **data, size_t *size) const {
        const Field *field = find(key);
        bool found = field != nullptr && field->type == cbor::Type::Bytes;
        if (found) {
            *data = field->data;
            *size = field->size;
        }

        return found;
    }

    /**
     * Boolean field: true only when present and true.
     */
    bool flag(int64_t key) const {
        const Field *field = find(key);
        return field != nullptr && field->type == cbor::Type::True;
    }

    /**
     * Text field (not null terminated). False if missing or of another type.
     */
    bool text(int64_t key, const char **text, size_t *length) const {
        const Field *field = find(key);
        bool found = field != nullptr && field->type == cbor::Type::Text;
        if (found) {
            *text = (const char *)field->data;
            *length = field->size;
        }

        return found;
    }
};

namespace {

// Bytes of a LOG message around its text: [LOG, 0, {STREAM: n, TEXT: <head>}].
const size_t LOG_OVERHEAD = 32;

/**
 * Starts a message [kind, id, payload]: the caller writes the payload.
 */
void writeHeader(cbor::Writer *writer, int kind, int64_t id) {
    writer->array(3);
    writer->integer(kind);
    writer->integer(id);
}

} // namespace

/**
 * Reads the payload of a request into 'fields'. False if malformed.
 */
bool Agent::readFields(cbor::Reader *reader, Fields *fields) {
    // no payload: no fields
    if (reader->peek() == cbor::Type::Null) {
        return reader->null();
    }

    // otherwise a map
    uint64_t count = 0;
    if (!reader->map(&count)) {
        return false;
    }

    // for each pair of the map
    for (uint64_t i = 0; i < count; i++) {
        // read the key (always an integer)
        Fields::Field item = { 0, cbor::Type::Invalid, 0, nullptr, 0 };
        if (!reader->integer(&item.key)) {
            return false;
        }

        // read the value: integers, text, bytes and booleans are kept,
        // other types are skipped (only their type is kept)
        item.type = reader->peek();
        bool ok = false;

        switch (item.type) {
            case cbor::Type::Unsigned:
            case cbor::Type::Negative: {
                ok = reader->integer(&item.integer);
                break;
            }
            case cbor::Type::Text: {
                const char *text = nullptr;
                ok = reader->text(&text, &item.size);
                item.data = (const uint8_t *)text;
                break;
            }
            case cbor::Type::Bytes: {
                ok = reader->bytes(&item.data, &item.size);
                break;
            }
            case cbor::Type::True:
            case cbor::Type::False: {
                item.integer = item.type == cbor::Type::True;
                ok = reader->skip();
                break;
            }
            default: {
                ok = reader->skip();
                break;
            }
        }

        if (!ok) {
            return false;
        }

        // store the field; more pairs than the table holds (16, no request
        // has that many) make the payload malformed rather than half read
        int capacity = (int)(sizeof(fields->items) / sizeof(fields->items[0]));
        if (fields->count == capacity) {
            return false;
        }

        fields->items[fields->count++] = item;
    }

    return true;
}

/**
 * An agent without host, session nor upload.
 */
Agent::Agent(Storage *storage, RamStorage *ram, Controller *controller, Identity identity, size_t maxMessage)
    // the frame parser keeps the frame of the largest message (its header
    // and the COBS overhead included); bytes that are not frames are
    // dropped (no text callback)
    : _storage(storage), _ram(ram), _uploadStorage(storage), _controller(controller), _debugger(nullptr), _identity(identity),
      _maxMessage(maxMessage), _parser(frames::maxSegmentSize(maxMessage), &onMessage, nullptr, this), _link(nullptr), _session(false),
      _uploading(false), _uploadSize(0), _uploadReceived(0), _hash(nullptr), _dataTotal(0), _agentAnswer{ -1, nullptr, 0 },
      _debuggerAnswer{ -1, nullptr, 0 } {
    _uploadBuildId[0] = 0;
}

/**
 * Frees the upload hash and the remembered last answers.
 */
Agent::~Agent() {
    delete _hash;
    forgetAnswer(&_agentAnswer);
    forgetAnswer(&_debuggerAnswer);
}

/**
 * A host connected (link) or disconnected (nullptr).
 */
void Agent::attach(Link *link) {
    // switch the link; the new host starts with HELLO and numbers its
    // requests from scratch: the last answers belong to the previous host
    _mutex.lock();
    _link = link;
    _session = false;
    forgetAnswer(&_agentAnswer);
    forgetAnswer(&_debuggerAnswer);
    _mutex.unlock();

    // a debugger session belongs to the host that opened it: end it
    if (_debugger != nullptr) {
        _debugger->hostGone();
    }
}

/**
 * True while a host session is open.
 */
bool Agent::sessionOpen() {
    _mutex.lock();
    bool open = _session;
    _mutex.unlock();
    return open;
}

/**
 * Bytes received from the host: onMessage() is called for each complete frame.
 */
void Agent::receive(const uint8_t *data, size_t size) {
    _parser.feed(data, size);
}

/**
 * One message from the host (transport thread).
 */
void Agent::onMessage(void *opaque, const uint8_t *message, size_t size) {
    Agent *agent = (Agent *)opaque;

    // decode [kind, id, payload]; anything else is not ours: ignored
    cbor::Reader reader(message, size);
    uint64_t count = 0;
    int64_t kind = 0;
    int64_t id = 0;
    Fields fields;
    bool valid = reader.array(&count) && count == 3 && reader.integer(&kind) && reader.integer(&id)
              && readFields(&reader, &fields);
    if (!valid) {
        return;
    }

    // same id as a last answered request: the host did not get the answer;
    // it is sent again, without doing the request again
    if (agent->resendAnswer(id)) {
        return;
    }

    // debugger request during a session: the debugger answers later, on
    // the JS thread (it also drops a request repeated before its answer)
    bool isDebugRequest = kind >= FIRST_DEBUG_KIND && kind <= LAST_DEBUG_KIND;
    if (isDebugRequest && agent->sessionOpen() && agent->_debugger != nullptr) {
        agent->_debugger->post(message, size);
        return;
    }

    // any other request: answered here, now
    agent->handle(kind, id, &fields);
}

/**
 * Runs a request and answers it.
 */
void Agent::handle(int64_t kind, int64_t id, const Fields *fields) {
    // HELLO is always accepted
    switch (kind) {
        case KIND_HELLO: {
            handleHello(id, fields);
            return;
        }
        default: {
            break;
        }
    }

    // every other request needs a session
    if (!sessionOpen()) {
        respondError(id, "no session: send HELLO first");
        return;
    }

    // dispatch by kind; unknown kinds (and debugger requests without
    // debugger) get an error
    switch (kind) {
        case KIND_INFO: {
            handleInfo(id);
            break;
        }
        case KIND_UPLOAD_BEGIN: {
            handleUploadBegin(id, fields);
            break;
        }
        case KIND_UPLOAD_CHUNK: {
            handleUploadChunk(id, fields);
            break;
        }
        case KIND_UPLOAD_END: {
            handleUploadEnd(id);
            break;
        }
        case KIND_RUN: {
            _controller->run();
            respondEmpty(id);
            break;
        }
        case KIND_STOP: {
            _controller->stop();
            respondEmpty(id);
            break;
        }
        case KIND_RESET: {
            // answer first: the answer must go out before the restart
            respondEmpty(id);
            _controller->reset();
            break;
        }
        case KIND_FILE_LIST: {
            handleFileList(id, fields);
            break;
        }
        case KIND_FILE_READ: {
            handleFileRead(id, fields);
            break;
        }
        case KIND_FILE_WRITE_BEGIN:
        case KIND_FILE_WRITE_CHUNK:
        case KIND_FILE_WRITE_END: {
            handleFileWrite(kind, id, fields);
            break;
        }
        case KIND_FILE_REMOVE:
        case KIND_FILE_MKDIR:
        case KIND_FILE_RENAME: {
            handleFileChange(kind, id, fields);
            break;
        }
        default: {
            respondError(id, "unknown request");
            break;
        }
    }
}

/**
 * HELLO: opens the session.
 */
void Agent::handleHello(int64_t id, const Fields *fields) {
    // the host must speak our protocol version
    int64_t protocol = 0;
    if (!fields->integer(KEY_PROTOCOL, &protocol) || protocol != PROTOCOL_VERSION) {
        respondError(id, "unsupported protocol version");
        return;
    }

    // if the agent has a token (network links), the host must give it
    if (_identity.token != nullptr) {
        const char *token = nullptr;
        size_t length = 0;
        bool valid = fields->text(KEY_TOKEN, &token, &length) && length == strlen(_identity.token)
                  && memcmp(token, _identity.token, length) == 0;
        if (!valid) {
            respondError(id, "invalid token");
            return;
        }
    }

    // open the session
    _mutex.lock();
    _session = true;
    _mutex.unlock();

    // answer who we are: protocol, agent name, qjsx and engine versions,
    // maximum message size
    cbor::Writer payload;
    payload.integer(KEY_PROTOCOL);
    payload.integer(PROTOCOL_VERSION);
    payload.integer(KEY_AGENT);
    payload.text(_identity.name);
    payload.integer(KEY_ENGINE);
    payload.text(_identity.engine);
    payload.integer(KEY_VERSION);
    payload.text(_identity.version);
    payload.integer(KEY_MAX_MESSAGE);
    payload.integer((int64_t)_maxMessage);

    // and the native modules of this firmware a program may import (the
    // twin of a board: those of the board): the host refuses to deploy a
    // program that imports another one
    int registered = rt::countNativeModules();
    int nativeCount = 0;
    for (int i = 0; i < registered; i++) {
        nativeCount += rt::isDeviceNativeModule(rt::nativeModuleNameAt(i)) ? 1 : 0;
    }

    payload.integer(KEY_NATIVES);
    payload.array((uint64_t)nativeCount);

    for (int i = 0; i < registered; i++) {
        const char *name = rt::nativeModuleNameAt(i);
        if (rt::isDeviceNativeModule(name)) {
            payload.text(name);
        }
    }

    respond(id, &payload, 6);
}

/**
 * INFO: is a program running, which package RUN starts: the one loaded in
 * RAM if any (VOLATILE true), else the one installed in flash; the data
 * folder, the JS heap of the program and the free memory of the board.
 */
void Agent::handleInfo(int64_t id) {
    // the package in RAM first, else the installed one
    char buildId[64];
    size_t packageSize = 0;
    bool inRam = _ram->installed(buildId, sizeof(buildId), &packageSize);
    bool installed = inRam || _storage->installed(buildId, sizeof(buildId), &packageSize);

    // answer RUNNING, BUILD_ID (null without package), PACKAGE_SIZE (0
    // without package), VOLATILE
    cbor::Writer payload;
    payload.integer(KEY_VOLATILE);
    payload.boolean(inRam);
    payload.integer(KEY_RUNNING);
    payload.boolean(_controller->running());
    payload.integer(KEY_BUILD_ID);
    if (installed) {
        payload.text(buildId);
    } else {
        payload.null();
    }

    payload.integer(KEY_PACKAGE_SIZE);
    payload.integer(installed ? (int64_t)packageSize : 0);
    int fieldCount = 4;

    // the data folder: what its files use, and its size when known
    uint64_t used = 0;
    if (_files.usedBytes(&used) == 0) {
        payload.integer(KEY_DATA_USED);
        payload.integer((int64_t)used);
        fieldCount++;
    }
    if (_dataTotal > 0) {
        payload.integer(KEY_DATA_TOTAL);
        payload.integer((int64_t)_dataTotal);
        fieldCount++;
    }

    // the JS heap of the program: now, and the most of this run (or of the
    // last one)
    size_t heapUsed = 0;
    size_t heapPeak = 0;
    _controller->jsHeap(&heapUsed, &heapPeak);
    payload.integer(KEY_JS_HEAP_USED);
    payload.integer((int64_t)heapUsed);
    payload.integer(KEY_JS_HEAP_PEAK);
    payload.integer((int64_t)heapPeak);
    fieldCount += 2;

    // the free memory of the board, when it can tell; PSRAM only if it has some
    board::MemoryStatus memory = {};
    if (board::memoryStatus(&memory)) {
        payload.integer(KEY_RAM_FREE);
        payload.integer((int64_t)memory.ramFree);
        payload.integer(KEY_RAM_LARGEST);
        payload.integer((int64_t)memory.ramLargest);
        payload.integer(KEY_RAM_LOWEST);
        payload.integer((int64_t)memory.ramLowest);
        fieldCount += 3;

        if (memory.psramFree > 0) {
            payload.integer(KEY_PSRAM_FREE);
            payload.integer((int64_t)memory.psramFree);
            payload.integer(KEY_PSRAM_LARGEST);
            payload.integer((int64_t)memory.psramLargest);
            fieldCount += 2;
        }

        // the region reserved for the JS heap, when the board has one
        if (memory.jsHeapSize > 0) {
            payload.integer(KEY_JS_HEAP_SIZE);
            payload.integer((int64_t)memory.jsHeapSize);
            fieldCount++;
        }
    }

    respond(id, &payload, fieldCount);
}

/**
 * UPLOAD_BEGIN: a package of SIZE bytes is coming, to the flash, or to RAM
 * with VOLATILE. Refused if it was compiled for another bytecode version
 * than the one of this engine, or if the RAM cannot hold it.
 */
void Agent::handleUploadBegin(int64_t id, const Fields *fields) {
    // fields: SIZE > 0, SHA256 of 32 bytes, BUILD_ID that fits _uploadBuildId
    // (BYTECODE is checked below)
    int64_t size = 0;
    const uint8_t *hash = nullptr;
    size_t hashSize = 0;
    const char *buildId = nullptr;
    size_t buildIdLength = 0;
    bool valid = fields->integer(KEY_SIZE, &size) && size > 0 && fields->bytes(KEY_SHA256, &hash, &hashSize)
              && hashSize == 32 && fields->text(KEY_BUILD_ID, &buildId, &buildIdLength)
              && buildIdLength < sizeof(_uploadBuildId);
    if (!valid) {
        respondError(id, "UPLOAD_BEGIN: SIZE, SHA256 (32 bytes) and BUILD_ID expected");
        return;
    }

    // the package must hold the bytecode this engine reads: refused now,
    // before anything is written, so that the installed package stays
    int64_t bytecode = 0;
    if (!fields->integer(KEY_BYTECODE, &bytecode)) {
        respondError(id, "UPLOAD_BEGIN: BYTECODE expected");
        return;
    }

    if (bytecode != _identity.bytecode) {
        char message[160];
        snprintf(message, sizeof(message),
                 "the package holds bytecode version %d, this device reads version %d: "
                 "rebuild the package, or flash a firmware that matches qjsx",
                 (int)bytecode, _identity.bytecode);
        respondError(id, message);
        return;
    }

    // an upload in progress: the host started over, drop it
    if (_uploading) {
        _uploadStorage->abortUpload();
    }

    // where it goes: RAM (VOLATILE) or flash; the storage prepares room
    bool toRam = fields->flag(KEY_VOLATILE);
    _uploadStorage = toRam ? (Storage *)_ram : _storage;
    if (!_uploadStorage->beginUpload((size_t)size)) {
        _uploading = false;
        if (toRam) {
            char message[160];
            snprintf(message, sizeof(message),
                     "not enough RAM for this package (%u bytes): install it in flash with qjsx deploy",
                     (unsigned)size);
            respondError(id, message);
        } else {
            respondError(id, "cannot store a package of this size");
        }

        return;
    }

    // remember what the upload must end with: size, hash, build id
    _uploading = true;
    _uploadSize = (size_t)size;
    _uploadReceived = 0;
    memcpy(_uploadHash, hash, 32);
    memcpy(_uploadBuildId, buildId, buildIdLength);
    _uploadBuildId[buildIdLength] = 0;

    // start the hash of the received bytes
    delete _hash;
    _hash = new crypto::Sha256();
    respondEmpty(id);
}

/**
 * UPLOAD_CHUNK: the next bytes of the package.
 */
void Agent::handleUploadChunk(int64_t id, const Fields *fields) {
    // an upload must be in progress, with OFFSET and DATA
    int64_t offset = 0;
    const uint8_t *data = nullptr;
    size_t size = 0;
    if (!_uploading || !fields->integer(KEY_OFFSET, &offset) || !fields->bytes(KEY_DATA, &data, &size)) {
        respondError(id, "UPLOAD_CHUNK: no upload in progress, or OFFSET / DATA missing");
        return;
    }

    // chunks come in order and stay within the announced size (a chunk
    // sent again was answered by the repeat check of onMessage)
    if ((size_t)offset != _uploadReceived || _uploadReceived + size > _uploadSize) {
        respondError(id, "UPLOAD_CHUNK: unexpected offset or size");
        return;
    }

    // write the chunk; on error the upload stays in progress: the host may
    // send the chunk again
    if (!_uploadStorage->writeUpload(_uploadReceived, data, size)) {
        respondError(id, "cannot write the package");
        return;
    }

    // add it to the hash
    _hash->update(data, size);
    _uploadReceived += size;
    respondEmpty(id);
}

/**
 * UPLOAD_END: checks the package and installs it.
 */
void Agent::handleUploadEnd(int64_t id) {
    // every byte must have arrived
    if (!_uploading || _uploadReceived != _uploadSize) {
        respondError(id, "UPLOAD_END: incomplete upload");
        return;
    }

    // the hash of the received bytes must be the announced one, else drop
    // the package
    uint8_t digest[32];
    _hash->finish(digest);
    _uploading = false;
    if (memcmp(digest, _uploadHash, 32) != 0) {
        _uploadStorage->abortUpload();
        respondError(id, "UPLOAD_END: SHA-256 mismatch");
        return;
    }

    // install it with its build id
    if (!_uploadStorage->finishUpload(_uploadBuildId)) {
        respondError(id, "cannot install the package");
        return;
    }

    // a package installed in flash replaces the one loaded in RAM: RUN
    // starts what was just installed
    if (_uploadStorage == _storage) {
        _ram->clear();
    }

    // answer the build id now installed
    cbor::Writer payload;
    payload.integer(KEY_BUILD_ID);
    payload.text(_uploadBuildId);
    respond(id, &payload, 1);
}

// ### Files of the data folder ################################################

/**
 * FILE_LIST: PATH -> ENTRIES [[name, size, isDirectory]].
 */
void Agent::handleFileList(int64_t id, const Fields *fields) {
    // the folder (no PATH: the data folder itself)
    const char *path = "";
    size_t length = 0;
    fields->text(KEY_PATH, &path, &length);

    // its entries
    FileEntry *entries = nullptr;
    int count = 0;
    int error = _files.list(path, length, &entries, &count);
    if (error != 0) {
        respondFileError(id, error, path, length);
        return;
    }

    // as an array of [name, size, isDirectory]
    cbor::Writer payload;
    payload.integer(KEY_ENTRIES);
    payload.array((uint64_t)count);
    for (int index = 0; index < count; index++) {
        payload.array(3);
        payload.text(entries[index].name);
        payload.integer(entries[index].size);
        payload.boolean(entries[index].isDirectory);
    }

    DataFiles::freeEntries(entries, count);
    respond(id, &payload, 1);
}

/**
 * FILE_READ: PATH, OFFSET -> DATA (FILE_CHUNK bytes at most), SIZE.
 */
void Agent::handleFileRead(int64_t id, const Fields *fields) {
    const char *path = nullptr;
    size_t length = 0;
    int64_t offset = 0;
    if (!fields->text(KEY_PATH, &path, &length) || !fields->integer(KEY_OFFSET, &offset)) {
        respondError(id, "FILE_READ: PATH and OFFSET expected");
        return;
    }

    // the piece, in a buffer on the heap (thread stacks are small)
    uint8_t *buffer = (uint8_t *)memory::allocate(FILE_CHUNK);
    size_t got = 0;
    int64_t fileSize = 0;
    int error = _files.read(path, length, offset, buffer, FILE_CHUNK, &got, &fileSize);
    if (error != 0) {
        memory::release(buffer);
        respondFileError(id, error, path, length);
        return;
    }

    cbor::Writer payload;
    payload.integer(KEY_DATA);
    payload.bytes(buffer, got);
    payload.integer(KEY_SIZE);
    payload.integer(fileSize);
    memory::release(buffer);
    respond(id, &payload, 2);
}

/**
 * FILE_WRITE_BEGIN (PATH, SIZE), FILE_WRITE_CHUNK (OFFSET, DATA),
 * FILE_WRITE_END: one file at a time, as the upload of a package.
 */
void Agent::handleFileWrite(int64_t kind, int64_t id, const Fields *fields) {
    // begin: where, how much
    if (kind == KIND_FILE_WRITE_BEGIN) {
        const char *path = nullptr;
        size_t length = 0;
        int64_t size = 0;
        if (!fields->text(KEY_PATH, &path, &length) || !fields->integer(KEY_SIZE, &size)) {
            respondError(id, "FILE_WRITE_BEGIN: PATH and SIZE expected");
            return;
        }

        int error = _files.beginWrite(path, length, size);
        if (error != 0) {
            respondFileError(id, error, path, length);
            return;
        }
        respondEmpty(id);
        return;
    }

    // a chunk, in order
    if (kind == KIND_FILE_WRITE_CHUNK) {
        int64_t offset = 0;
        const uint8_t *data = nullptr;
        size_t size = 0;
        if (!fields->integer(KEY_OFFSET, &offset) || !fields->bytes(KEY_DATA, &data, &size)) {
            respondError(id, "FILE_WRITE_CHUNK: OFFSET and DATA expected");
            return;
        }

        int error = _files.writeChunk(offset, data, size);
        if (error != 0) {
            respondFileError(id, error, "", 0);
            return;
        }
        respondEmpty(id);
        return;
    }

    // the end: the file in place
    int error = _files.endWrite();
    if (error != 0) {
        respondFileError(id, error, "", 0);
        return;
    }
    respondEmpty(id);
}

/**
 * FILE_REMOVE (PATH), FILE_MKDIR (PATH), FILE_RENAME (PATH, TO).
 */
void Agent::handleFileChange(int64_t kind, int64_t id, const Fields *fields) {
    const char *path = nullptr;
    size_t length = 0;
    if (!fields->text(KEY_PATH, &path, &length)) {
        respondError(id, "PATH expected");
        return;
    }

    // the operation
    int error = 0;
    if (kind == KIND_FILE_REMOVE) {
        error = _files.remove(path, length);
    } else if (kind == KIND_FILE_MKDIR) {
        error = _files.makeDirectory(path, length);
    } else {
        const char *to = nullptr;
        size_t toLength = 0;
        if (!fields->text(KEY_TO, &to, &toLength)) {
            respondError(id, "FILE_RENAME: TO expected");
            return;
        }
        error = _files.rename(path, length, to, toLength);
    }

    if (error != 0) {
        respondFileError(id, error, path, length);
        return;
    }
    respondEmpty(id);
}

/**
 * [ERROR, id, {MESSAGE, ERROR_CODE}], kept as the agent's last answer.
 */
void Agent::respondFileError(int64_t id, int error, const char *path, size_t length) {
    // "ENOENT: No such file or directory, 'notes.txt'" (the path cut if long)
    const char *code = js::detail::errnoName(error);
    int shown = (int)(length > 160 ? 160 : length);
    char text[256];
    if (length > 0) {
        snprintf(text, sizeof(text), "%s: %s, '%.*s'", code, strerror(error), shown, path);
    } else {
        snprintf(text, sizeof(text), "%s: %s", code, strerror(error));
    }

    cbor::Writer message;
    writeHeader(&message, KIND_ERROR, id);
    message.map(2);
    message.integer(KEY_MESSAGE);
    message.text(text);
    message.integer(KEY_ERROR_CODE);
    message.text(code);
    sendMessage(message.data(), message.size(), id, &_agentAnswer);
}

// ### Sending #################################################################

/**
 * Sends a message in a frame; an answer is kept in 'answer' for a repeated
 * request (nullptr: an event, not kept). Any thread.
 */
void Agent::sendMessage(const uint8_t *message, size_t size, int64_t requestId, LastAnswer *answer) {
    // encode the frame, outside the lock
    size_t frameSize = 0;
    uint8_t *frame = frames::encode(message, size, &frameSize);

    // send it under the lock: frames sent by different threads stay whole
    _mutex.lock();
    if (_link != nullptr) {
        _link->send(frame, frameSize);
    }

    // an answer: keep its frame as the last answer of its answerer (replaces
    // the previous one)
    if (answer != nullptr) {
        forgetAnswer(answer);
        answer->requestId = requestId;
        answer->frame = frame;
        answer->size = frameSize;
        frame = nullptr;
    }

    _mutex.unlock();

    // not kept: free it
    memory::release(frame);
}

/**
 * Frees the frame of a last answer and marks it empty.
 */
void Agent::forgetAnswer(LastAnswer *answer) {
    memory::release(answer->frame);
    answer->requestId = -1;
    answer->frame = nullptr;
    answer->size = 0;
}

/**
 * Sends again the last answer (of the agent or of the debugger) to request
 * 'id', if there is one. Any thread.
 */
bool Agent::resendAnswer(int64_t id) {
    // events (id 0) are never answered
    if (id == 0) {
        return false;
    }

    // one critical section: the answer cannot be replaced between the test
    // and the sending
    _mutex.lock();
    LastAnswer *answer = nullptr;
    if (_agentAnswer.frame != nullptr && _agentAnswer.requestId == id) {
        answer = &_agentAnswer;
    } else if (_debuggerAnswer.frame != nullptr && _debuggerAnswer.requestId == id) {
        answer = &_debuggerAnswer;
    }

    bool found = answer != nullptr;
    if (found && _link != nullptr) {
        _link->send(answer->frame, answer->size);
    }

    _mutex.unlock();
    return found;
}

/**
 * Sends [kind, id, {pairs}]: 'payload' holds 'fieldCount' encoded pairs; an
 * answer is kept in 'answer' (nullptr for an event).
 */
void Agent::sendWithPayload(int kind, int64_t id, const cbor::Writer *payload, int fieldCount, LastAnswer *answer) {
    // the message
    cbor::Writer message;
    writeHeader(&message, kind, id);
    message.map(fieldCount);
    message.encoded(payload->data(), payload->size());

    // too large for the link (the host could not take it, and its frame
    // would need two more copies here): an answer becomes an error, an
    // event is dropped
    if (message.size() > _maxMessage) {
        if (answer != nullptr) {
            char text[96];
            snprintf(text, sizeof(text), "answer too large for the link: %lu bytes, at most %lu",
                     (unsigned long)message.size(), (unsigned long)_maxMessage);
            sendError(answer, id, text);
        }

        return;
    }

    sendMessage(message.data(), message.size(), id, answer);
}

/**
 * Sends [ERROR, id, {MESSAGE}], kept in 'answer'.
 */
void Agent::sendError(LastAnswer *answer, int64_t id, const char *text) {
    cbor::Writer message;
    writeHeader(&message, KIND_ERROR, id);
    message.map(1);
    message.integer(KEY_MESSAGE);
    message.text(text);
    sendMessage(message.data(), message.size(), id, answer);
}

/**
 * Sends a RESPONSE of the agent with this payload.
 */
void Agent::respond(int64_t id, const cbor::Writer *payload, int fieldCount) {
    sendWithPayload(KIND_RESPONSE, id, payload, fieldCount, &_agentAnswer);
}

/**
 * Sends a RESPONSE of the agent with an empty map.
 */
void Agent::respondEmpty(int64_t id) {
    cbor::Writer payload;
    respond(id, &payload, 0);
}

/**
 * Sends an ERROR of the agent.
 */
void Agent::respondError(int64_t id, const char *text) {
    sendError(&_agentAnswer, id, text);
}

/**
 * Sends a RESPONSE of the debugger with this payload.
 */
void Agent::answerDebugRequest(int64_t id, const cbor::Writer *payload, int fieldCount) {
    sendWithPayload(KIND_RESPONSE, id, payload, fieldCount, &_debuggerAnswer);
}

/**
 * Sends an ERROR of the debugger.
 */
void Agent::answerDebugError(int64_t id, const char *text) {
    sendError(&_debuggerAnswer, id, text);
}

/**
 * Sends an event, during a session only.
 */
void Agent::sendEvent(int kind, const cbor::Writer *payload, int fieldCount) {
    if (sessionOpen()) {
        sendWithPayload(kind, 0, payload, fieldCount, nullptr);
    }
}

/**
 * Sends console output as [LOG, 0, {STREAM, TEXT}]. False without session:
 * the caller prints it locally.
 */
bool Agent::sendLog(int stream, const char *text, size_t length) {
    if (!sessionOpen()) {
        return false;
    }

    // one message per piece (one at least: an empty text is sent too)
    size_t pieceLimit = _maxMessage - LOG_OVERHEAD;
    size_t offset = 0;

    do {
        // the piece: what remains, or what fits, cut before a UTF-8
        // continuation byte (10xxxxxx) so that no character is split; a run
        // of continuation bytes as long as a piece is not text: cut anywhere
        size_t piece = length - offset;
        if (piece > pieceLimit) {
            piece = pieceLimit;
            while (piece > 0 && ((uint8_t)text[offset + piece] & 0xC0) == 0x80) {
                piece--;
            }

            if (piece == 0) {
                piece = pieceLimit;
            }
        }

        // [LOG, 0, {STREAM, TEXT}]
        cbor::Writer message;
        writeHeader(&message, KIND_LOG, 0);
        message.map(2);
        message.integer(KEY_STREAM);
        message.integer(stream);
        message.integer(KEY_TEXT);
        message.text(text + offset, piece);
        sendMessage(message.data(), message.size(), 0, nullptr);
        offset += piece;
    } while (offset < length);

    return true;
}

/**
 * Sends [EXITED, 0, {CODE}] during a session.
 */
void Agent::sendExited(int code) {
    cbor::Writer message;
    writeHeader(&message, KIND_EXITED, 0);
    message.map(1);
    message.integer(KEY_CODE);
    message.integer(code);

    if (sessionOpen()) {
        sendMessage(message.data(), message.size(), 0, nullptr);
    }
}

/**
 * Sends [STARTED, 0, {BUILD_ID}] during a session.
 */
void Agent::sendStarted(const char *buildId) {
    cbor::Writer message;
    writeHeader(&message, KIND_STARTED, 0);
    message.map(1);
    message.integer(KEY_BUILD_ID);
    message.text(buildId);

    if (sessionOpen()) {
        sendMessage(message.data(), message.size(), 0, nullptr);
    }
}

} // namespace agent
