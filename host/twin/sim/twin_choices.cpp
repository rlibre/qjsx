/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// The choices of the page kept in <storage>/twin.json (see twin.h, "Choices
// of the page"): one JSON object per key, kept as text, the file written
// again whole at each change:
//
//   {
//       "uart1": { "port": 8094, "listen": true },
//       "serial1": { "port": "COM5", "connect": true, "realSpeed": true }
//   }
//
// Threads: any; a lock protects the entries and the writing of the file.
#include "twin.h"
#include "files.h"
#include "memory.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

namespace twin {

namespace {

const int MAX_CHOICES = 32;                  // keys kept

// A key and its object.
struct Choice {
    char *key;                               // owned
    char *json;                              // the object as JSON text, owned
};

Choice choices[MAX_CHOICES];                 // lock
int choiceCount = 0;                         // lock: entries used
char *choicesPath = nullptr;                 // <storage>/twin.json, owned (set once, before any link)
platform::Mutex lock;                        // choices, the file

/**
 * The entry of 'key', or nullptr. Lock held.
 */
Choice *findChoice(const char *key) {
    for (int i = 0; i < choiceCount; i++) {
        if (strcmp(choices[i].key, key) == 0) {
            return &choices[i];
        }
    }

    return nullptr;
}

/**
 * Keeps 'json' for 'key' (a new entry, or replaces its text); false when
 * the table is full. Lock held.
 */
bool keepChoice(const char *key, const char *json) {
    Choice *choice = findChoice(key);
    if (choice == nullptr) {
        if (choiceCount >= MAX_CHOICES) {
            return false;
        }

        choice = &choices[choiceCount++];
        choice->key = memory::copyText(key, strlen(key));
        choice->json = nullptr;
    }

    if (choice->json != nullptr) {
        memory::release(choice->json);
    }

    choice->json = memory::copyText(json, strlen(json));
    return true;
}

/**
 * Writes the file from the entries (a failure is said: the twin goes on).
 * Lock held.
 */
void writeFile() {
    // the size: braces, and per entry the key, its object, quotes and spaces
    size_t size = 8;
    for (int i = 0; i < choiceCount; i++) {
        size += strlen(choices[i].key) + strlen(choices[i].json) + 16;
    }

    // { "key": object, ... }
    char *text = (char *)memory::allocate(size);
    size_t used = (size_t)snprintf(text, size, "{");
    for (int i = 0; i < choiceCount; i++) {
        const char *separator = i == 0 ? "" : ",";
        used += (size_t)snprintf(text + used, size - used, "%s\n    \"%s\": %s", separator, choices[i].key,
                                 choices[i].json);
    }

    used += (size_t)snprintf(text + used, size - used, "\n}\n");

    if (!files::writeAll(choicesPath, text, used)) {
        fprintf(stderr, "qjsx twin: cannot write %s\n", choicesPath);
    }

    memory::release(text);
}

} // namespace

/**
 * 1. the path of the file;
 * 2. the file parsed with a short-lived QuickJS context;
 * 3. each property that is an object kept as JSON text.
 */
void choicesLoad(const char *storage) {
    // 1. the path
    size_t pathSize = strlen(storage) + strlen("/twin.json") + 1;
    choicesPath = (char *)memory::allocate(pathSize);
    snprintf(choicesPath, pathSize, "%s/twin.json", storage);

    // 2. the file, if any
    size_t size = 0;
    uint8_t *text = files::readAll(choicesPath, &size);
    if (text == nullptr) {
        return;
    }

    char *json = (char *)memory::allocate(size + 1);
    memcpy(json, text, size);
    json[size] = 0;
    memory::release(text);

    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(runtime);
    JSValue file = JS_ParseJSON(ctx, json, size, choicesPath);
    memory::release(json);

    // 3. its objects, each by its key
    JSPropertyEnum *properties = nullptr;
    uint32_t count = 0;
    bool listed = JS_IsObject(file)
               && JS_GetOwnPropertyNames(ctx, &properties, &count, file, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0;
    lock.lock();
    for (uint32_t i = 0; listed && i < count; i++) {
        JSValue value = JS_GetProperty(ctx, file, properties[i].atom);
        const char *key = JS_AtomToCString(ctx, properties[i].atom);
        JSValue object = JS_IsObject(value) ? JS_JSONStringify(ctx, value, JS_UNDEFINED, JS_UNDEFINED) : JS_UNDEFINED;
        const char *objectText = JS_IsString(object) ? JS_ToCString(ctx, object) : nullptr;
        if (key != nullptr && objectText != nullptr) {
            keepChoice(key, objectText);
        }

        JS_FreeCString(ctx, objectText);
        JS_FreeValue(ctx, object);
        JS_FreeCString(ctx, key);
        JS_FreeValue(ctx, value);
    }

    lock.unlock();

    if (listed) {
        JS_FreePropertyEnum(ctx, properties, count);
    }

    JS_FreeValue(ctx, file);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
}

/**
 * Parses the text kept for 'key' with a short-lived context and gives the
 * object to 'read'.
 */
bool choicesRead(const char *key, ChoiceReader *read, void *context) {
    // a copy of the text, the lock released before any parsing
    lock.lock();
    Choice *choice = findChoice(key);
    char *json = choice != nullptr ? memory::copyText(choice->json, strlen(choice->json)) : nullptr;
    lock.unlock();
    if (json == nullptr) {
        return false;
    }

    // the object, to the reader
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(runtime);
    JSValue object = JS_ParseJSON(ctx, json, strlen(json), key);
    bool found = JS_IsObject(object);
    if (found) {
        read(context, ctx, object);
    }

    JS_FreeValue(ctx, object);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    memory::release(json);
    return found;
}

/**
 * Keeps the object and writes the file.
 */
void choicesWrite(const char *key, const char *json) {
    if (choicesPath == nullptr) {
        return;
    }

    lock.lock();
    if (keepChoice(key, json)) {
        writeFile();
    } else {
        fprintf(stderr, "qjsx twin: too many choices to keep, \"%s\" not saved\n", key);
    }

    lock.unlock();
}

} // namespace twin
