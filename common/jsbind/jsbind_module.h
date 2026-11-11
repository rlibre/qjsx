/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// jsbind - native modules and global objects.
//
//   JS_MODULE(fs, m)                       // import { readFileSync } from "sys/fs"
//   {
//       JS_FUNCTION(m, readFileSync);
//       JS_ADD_CLASS(m, Stats);
//   }
//
//   JS_GLOBALS(console, g)                 // globalThis.console = { log, ... }
//   {
//       g.object("console", [](js::ModuleBuilder &console) {
//           JS_FUNCTION(console, log);
//       });
//   }
//
// The body of a module runs twice: once to declare the export names (before
// the module is linked), once to create the values. Writing each export once
// avoids the classic mismatch between the two lists.
#pragma once

#include "jsbind_class.h"

namespace js {

/**
 * Receives the exports written in a JS_MODULE / JS_GLOBALS body. Depending
 * on the mode, each export only declares its name, sets a module export, or
 * defines a property of a JS object. Failures are remembered in ok().
 */
class ModuleBuilder {
public:
    enum class Mode {
        Declare,   // JS_AddModuleExport
        Define,    // JS_SetModuleExport
        Object,    // properties of a JS object (globals, namespaces)
    };

    /**
     * Builder for a module (Declare or Define); 'owner' prefixes the names
     * used in error messages ("fs.readFileSync").
     */
    ModuleBuilder(JSContext *ctx, JSModuleDef *module, Mode mode, const char *owner)
        : _ctx(ctx), _module(module), _target(JS_UNDEFINED), _mode(mode), _owner(owner), _ok(true) {
    }

    /**
     * Builder for the properties of 'target' (borrowed); 'owner' prefixes
     * the names used in error messages (nullptr: none).
     */
    ModuleBuilder(JSContext *ctx, JSValueConst target, const char *owner)
        : _ctx(ctx), _module(nullptr), _target(target), _mode(Mode::Object), _owner(owner), _ok(true) {
    }

    /**
     * False if an export failed.
     */
    bool         ok() const {
        return _ok;
    }

    /**
     * Context the exports are created in.
     */
    JSContext   *context() const {
        return _ctx;
    }

    /**
     * Exports the free C++ function F under 'name'.
     */
    template<auto F>
    void function(const char *name) {
        // Declare mode: only the name
        if (_mode == Mode::Declare) {
            declare(name);
            return;
        }

        // a JS function calling functionTrampoline<F>, whose 'magic' is the
        // registered name "owner.name"
        using Arguments = typename detail::FunctionTraits<decltype(F)>::Arguments;
        int arity = detail::JsArity<Arguments>::value;
        int nameIndex = detail::registerName(_owner, name);
        _export(name, JS_NewCFunctionMagic(_ctx, &detail::functionTrampoline<F>, name, arity,
                                           JS_CFUNC_generic_magic, nameIndex));
    }

    /**
     * Exports the JS constructor of a class (named after the C++ class).
     */
    template<class T>
    void cls() {
        // Declare mode: only the name
        const char *className = detail::ClassAccess<T>::name();
        if (_mode == Mode::Declare) {
            declare(className);
            return;
        }

        // the constructor (its prototype is created if needed)
        _export(className, detail::ClassAccess<T>::constructor(_ctx));
    }

    /**
     * Exports a constant value.
     */
    template<class T>
    void constant(const char *name, const T &value) {
        // Declare mode: only the name
        if (_mode == Mode::Declare) {
            declare(name);
            return;
        }

        // the value converted to JS
        _export(name, Convert<T>::to(_ctx, value));
    }

    /**
     * String constants (a string literal would otherwise be deduced as an array).
     */
    void constant(const char *name, const char *value) {
        constant<const char *>(name, value);
    }

    /**
     * Exports a plain object whose properties are defined by 'body', a
     * function taking a ModuleBuilder &.
     */
    template<class Body>
    void object(const char *name, Body body) {
        // Declare mode: only the name, 'body' does not run
        if (_mode == Mode::Declare) {
            declare(name);
            return;
        }

        // a new object ('body' does not run if it cannot be made)
        JSValue object = JS_NewObject(_ctx);
        if (JS_IsException(object)) {
            _ok = false;
            return;
        }

        // fill it through an inner builder ('name' prefixes its error
        // messages), then export it
        ModuleBuilder inner(_ctx, object, name);
        body(inner);
        _ok = _ok && inner.ok();
        _export(name, object);
    }

private:
    /**
     * Declares the export name 'name' of the module (Declare mode).
     */
    void declare(const char *name) {
        if (JS_AddModuleExport(_ctx, _module, name) < 0) {
            _ok = false;
        }
    }

    /**
     * Gives 'value' (owned) to the export or property 'name'. JS_EXCEPTION only
     * marks the builder as failed.
     */
    void _export(const char *name, JSValue value) {
        // the value could not be created: remember the failure
        if (JS_IsException(value)) {
            _ok = false;
            return;
        }

        // Define mode: module export; Object mode: property of the target
        // (QuickJS takes the value in both cases, even on failure)
        int result = 0;
        if (_mode == Mode::Define) {
            result = JS_SetModuleExport(_ctx, _module, name, value);
        } else {
            result = JS_DefinePropertyValueStr(_ctx, _target, name, value, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        }

        // remember a failure
        if (result < 0) {
            _ok = false;
        }
    }

    JSContext   *_ctx;     // context of the module
    JSModuleDef *_module;  // module receiving the exports; nullptr in Object mode
    JSValueConst _target;  // object receiving the properties (Object mode); borrowed
    Mode         _mode;    // what an export does
    const char  *_owner;   // prefix of names in error messages; nullptr: none
    bool         _ok;      // false once an export failed
};

} // namespace js

/**
 * Defines 'JSModuleDef *jsNewModule_<name>(JSContext *ctx, const char *moduleName)'.
 * jsNewModule_<name> creates the C module and runs the body in Declare mode;
 * when QuickJS later initializes the module, jsModuleInit_<name> runs the
 * body again in Define mode. The macro ends with the head of the body
 * function: the braces written after JS_MODULE(...) are its body, with
 * 'builder' as the ModuleBuilder.
 */
#define JS_MODULE(name, builder)                                                    \
    static void jsModuleBody_##name(::js::ModuleBuilder &builder);                  \
    static int jsModuleInit_##name(JSContext *ctx, JSModuleDef *module)             \
    {                                                                               \
        ::js::ModuleBuilder definer(ctx, module, ::js::ModuleBuilder::Mode::Define, #name); \
        jsModuleBody_##name(definer);                                               \
        return definer.ok() ? 0 : -1;                                               \
    }                                                                               \
    JSModuleDef *jsNewModule_##name(JSContext *ctx, const char *moduleName)         \
    {                                                                               \
        JSModuleDef *module = JS_NewCModule(ctx, moduleName, jsModuleInit_##name);  \
        if (module == nullptr) {                                                    \
            return nullptr;                                                         \
        }                                                                           \
        ::js::ModuleBuilder declarer(ctx, module, ::js::ModuleBuilder::Mode::Declare, #name); \
        jsModuleBody_##name(declarer);                                              \
        return declarer.ok() ? module : nullptr;                                    \
    }                                                                               \
    static void jsModuleBody_##name(::js::ModuleBuilder &builder)

/**
 * Defines 'bool jsInstallGlobals_<name>(JSContext *ctx)', which runs the body
 * once in Object mode on the global object. As with JS_MODULE, the braces
 * written after JS_GLOBALS(...) are the body.
 */
#define JS_GLOBALS(name, builder)                                                   \
    static void jsGlobalsBody_##name(::js::ModuleBuilder &builder);                 \
    bool jsInstallGlobals_##name(JSContext *ctx)                                    \
    {                                                                               \
        JSValue global = JS_GetGlobalObject(ctx);                                   \
        ::js::ModuleBuilder definer(ctx, global, nullptr);                          \
        jsGlobalsBody_##name(definer);                                              \
        JS_FreeValue(ctx, global);                                                  \
        return definer.ok();                                                        \
    }                                                                               \
    static void jsGlobalsBody_##name(::js::ModuleBuilder &builder)

/**
 * Exports the free function 'fn' under its C++ name.
 */
#define JS_FUNCTION(builder, fn) (builder).template function<&fn>(#fn)

/**
 * Same, with a JS name different from the C++ name (when the C++ name would
 * clash, e.g. with log() from <math.h>).
 */
#define JS_FUNCTION_AS(builder, fn, jsName) (builder).template function<&fn>(jsName)

/**
 * Exports the JS constructor of a bound class.
 */
#define JS_ADD_CLASS(builder, Class) (builder).template cls<Class>()
