// Module "crypto": sha256(data) -> Uint8Array(32).
// data: a string (hashed as UTF-8) or bytes (Uint8Array, typed array, ArrayBuffer).
#include <string.h>

#include "jsbind.h"
#include "sha256.h"

namespace {

/**
 * crypto.sha256(data): the SHA-256 digest of 'data' (a string, hashed as
 * UTF-8, or bytes) in a new 32-byte Uint8Array.
 */
js::Buffer sha256(js::Value data) {
    JSContext *ctx = js::context();
    crypto::Sha256 hash;

    // a string: hash its UTF-8 bytes; a failed conversion leaves its JS
    // exception pending
    if (JS_IsString(data.get())) {
        size_t size = 0;
        const char *text = JS_ToCStringLen(ctx, &size, data.get());
        if (text == nullptr) {
            js::detail::markPendingException();
            return js::Buffer();
        }

        hash.update((const uint8_t *)text, size);
        JS_FreeCString(ctx, text);
    } else {
        // bytes (typed array or ArrayBuffer): hashed in place, the reference
        // in 'buffer' keeping the memory alive until then; anything else is a
        // TypeError
        uint8_t *bytes = nullptr;
        size_t size = 0;
        JSValue buffer = JS_UNDEFINED;
        if (!js::detail::bytesOf(ctx, data.get(), &bytes, &size, &buffer)) {
            return js::fail<js::TypeError>("sha256: data must be a string or a Uint8Array");
        }

        hash.update(bytes, size);
        JS_FreeValue(ctx, buffer);
    }

    // copy the digest into the returned array
    uint8_t digest[32];
    hash.finish(digest);
    return js::Buffer::copy(digest, sizeof(digest));
}

} // namespace

/**
 * The "crypto" module: sha256.
 */
JS_MODULE(crypto, module) {
    JS_FUNCTION(module, sha256);
}
