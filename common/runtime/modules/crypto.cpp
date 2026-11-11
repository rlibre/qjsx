/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Module "sys/crypto":
//   sha256(data) -> Uint8Array(32)
//   sha1(data)   -> Uint8Array(20)   (only for what a protocol imposes: SHA-1 is broken)
// data: a string (hashed as UTF-8) or bytes (Uint8Array, typed array, ArrayBuffer).
// Base64 is not here: the engine has the standard Uint8Array.prototype.toBase64()
// and Uint8Array.fromBase64().
#include <string.h>

#include "jsbind.h"
#include "sha1.h"
#include "sha256.h"

namespace {

/**
 * Feeds 'data' (a string, hashed as UTF-8, or bytes) to 'hash'. False with
 * a pending JS exception if 'data' is neither ('name': the JS function, for
 * the message).
 */
template <typename Hash>
bool hashData(js::Value data, Hash *hash, const char *name) {
    JSContext *ctx = js::context();

    // a string: hash its UTF-8 bytes; a failed conversion leaves its JS
    // exception pending
    if (JS_IsString(data.get())) {
        size_t size = 0;
        const char *text = JS_ToCStringLen(ctx, &size, data.get());
        if (text == nullptr) {
            js::detail::markPendingException();
            return false;
        }

        hash->update((const uint8_t *)text, size);
        JS_FreeCString(ctx, text);
        return true;
    }

    // bytes (typed array or ArrayBuffer): hashed in place, the reference in
    // 'buffer' keeping the memory alive until then; anything else is a
    // TypeError
    uint8_t *bytes = nullptr;
    size_t size = 0;
    JSValue buffer = JS_UNDEFINED;
    if (!js::detail::bytesOf(ctx, data.get(), &bytes, &size, &buffer)) {
        (void)js::fail<js::TypeError>("%s: data must be a string or a Uint8Array", name);
        return false;
    }

    hash->update(bytes, size);
    JS_FreeValue(ctx, buffer);
    return true;
}

/**
 * crypto.sha256(data): the SHA-256 digest of 'data' in a new 32-byte
 * Uint8Array.
 */
js::Buffer sha256(js::Value data) {
    // hash the data (the error is pending if it is neither text nor bytes)
    crypto::Sha256 hash;
    if (!hashData(data, &hash, "sha256")) {
        return js::Buffer();
    }

    // copy the digest into the returned array
    uint8_t digest[32];
    hash.finish(digest);
    return js::Buffer::copy(digest, sizeof(digest));
}

/**
 * crypto.sha1(data): the SHA-1 digest of 'data' in a new 20-byte Uint8Array.
 */
js::Buffer sha1(js::Value data) {
    // hash the data (the error is pending if it is neither text nor bytes)
    crypto::Sha1 hash;
    if (!hashData(data, &hash, "sha1")) {
        return js::Buffer();
    }

    // copy the digest into the returned array
    uint8_t digest[20];
    hash.finish(digest);
    return js::Buffer::copy(digest, sizeof(digest));
}

} // namespace

/**
 * The "sys/crypto" module: sha256, sha1.
 */
JS_MODULE(crypto, module) {
    JS_FUNCTION(module, sha256);
    JS_FUNCTION(module, sha1);
}
