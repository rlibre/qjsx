/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Native module "draw_test": runs the renderer of the displays (common/draw)
// into memory, without a display (tests/image.test.js). Only built into
// qjsx-tests.
//
//   renderList(items, images, width, height, background, originX, originY)
//     -> Uint8Array of width x height pixels, 2 bytes each, high byte first:
//     the background, then the commands of 'items' (an Int16Array, as
//     sys/draw takes them) drawn into a target placed at (originX, originY)
//     in the drawing; 'images' are the image files of the image commands.
#include <string.h>

#include "draw.h"
#include "jsbind.h"
#include "memory.h"
#include "modules.h"

namespace {

/**
 * The bytes of a typed array, borrowed (nullptr with a TypeError when it is
 * not one).
 */
const uint8_t *bytesOf(js::Value value, size_t *size) {
    JSContext *ctx = js::context();
    uint8_t *bytes = nullptr;
    JSValue buffer = JS_UNDEFINED;
    if (!js::detail::bytesOf(ctx, value.get(), &bytes, size, &buffer)) {
        js::fail<js::TypeError>("draw_test: a typed array was expected");
        return nullptr;
    }

    JS_FreeValue(ctx, buffer);
    return bytes;
}

/**
 * JS: renderList(...): see the top of the file. An image that is not one is
 * a TypeError.
 */
js::Value renderList(js::Value items, js::Value images, int width, int height, int background, int originX, int originY) {
    JSContext *ctx = js::context();

    // the commands
    size_t itemSize = 0;
    const uint8_t *itemBytes = bytesOf(items, &itemSize);
    if (itemBytes == nullptr) {
        return js::Value();
    }

    // the images, read in place
    draw::Image read[draw::MAX_IMAGES];
    int imageCount = 0;
    JSValue length = JS_GetPropertyStr(ctx, images.get(), "length");
    JS_ToInt32(ctx, &imageCount, length);
    JS_FreeValue(ctx, length);
    if (imageCount > draw::MAX_IMAGES) {
        imageCount = draw::MAX_IMAGES;
    }

    for (int index = 0; index < imageCount; index++) {
        js::Value image(ctx, JS_GetPropertyUint32(ctx, images.get(), (uint32_t)index));
        size_t size = 0;
        const uint8_t *bytes = bytesOf(image, &size);
        if (bytes == nullptr) {
            return js::Value();
        }

        if (!draw::openImage(bytes, size, &read[index])) {
            return js::fail<js::TypeError>("draw_test: image %d is not an image of the renderer", index);
        }
    }

    // the target: the background, then the commands
    size_t pixelBytes = (size_t)width * (size_t)height * 2;
    uint8_t *pixels = (uint8_t *)memory::allocate(pixelBytes);
    draw::Target target = { pixels, width, height, originX, originY };
    draw::fillRect(&target, originX, originY, width, height, (uint16_t)background);
    draw::render((const int16_t *)itemBytes, (int)(itemSize / 2), nullptr, 0, read, imageCount, &target);

    // as a Uint8Array over a copy (JS_NewTypedArray reads its 3 arguments)
    JSValue buffer = JS_NewArrayBufferCopy(ctx, pixels, pixelBytes);
    memory::release(pixels);
    JSValue arguments[3] = { buffer, JS_NewInt32(ctx, 0), JS_NewInt64(ctx, (int64_t)pixelBytes) };
    js::Value result(ctx, JS_NewTypedArray(ctx, 3, arguments, JS_TYPED_ARRAY_UINT8));
    JS_FreeValue(ctx, buffer);
    return result;
}

} // namespace

/**
 * Module "draw_test".
 */
JS_MODULE(draw_test, module) {
    JS_FUNCTION(module, renderList);
}

/**
 * Registers the module "draw_test"; called by registerTestModules().
 */
void registerDrawTestModule() {
    rt::registerNativeModule("draw_test", &jsNewModule_draw_test);
}
