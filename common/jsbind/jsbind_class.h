/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// jsbind - C++ classes visible from JS.
//
//   class Uart {
//       JS_CLASS(Uart);
//   public:
//       void init(Port port);                       // optional: JS constructor arguments
//       JS_EXPORT(write) void write(js::Str text);  // method
//       JS_EXPORT(speed) int speed = 0;             // property (read-only if const)
//   };
//
// How the markers work: JS_CLASS remembers the value of __COUNTER__ (a
// preprocessor counter that increases at each use). Each JS_EXPORT declares
// a static function 'jsRegister' overloaded on Slot<number of the marker>,
// whose body adds the member to the JS prototype. A member function body is
// compiled once the class is complete, so the marker can take the address of
// the member declared just after it. At registration, ClassAccess tries the
// slots 1..64 and calls the overloads that exist.
//
// Object lifetime: the C++ object lives inside the memory of the JS object
// (allocated with the QuickJS allocator); it is constructed without
// arguments, then 'init' is called with the JS constructor arguments, and it
// is destroyed by the JS finalizer. The destructor must not run JS.
//
// Call flow, JS -> C++: QuickJS calls a trampoline (methodTrampoline,
// getterTrampoline, setterTrampoline, construct, functionTrampoline) with the
// raw JS arguments and a 'magic' number, the index of the function's name in
// detail::names. The trampoline opens a CallScope, finds the C++ object behind
// 'this', converts each argument with Convert<A>::from into an ArgPack, calls
// the C++ member, and converts the result with Convert<R>::to. If an error was
// recorded at any step (conversion, js::fail, exception thrown by JS called
// from C++), it returns JS_EXCEPTION and the pending exception reaches JS.
#pragma once

#include <stddef.h>
#include "jsbind_convert.h"
#include "memory.h"

namespace js {
namespace detail {

template<class T> struct ClassId {
    static inline JSClassID id = 0;  // QuickJS class id of T; 0: not registered yet
};

/**
 * Memory block of a JS object's C++ side: the header, then the C++ object.
 * It is the opaque pointer of the JS object.
 */
template<class T>
struct Instance {
    InstanceHeader header;                        // binding state of the object
    alignas(T) unsigned char storage[sizeof(T)];  // the C++ object

    /**
     * The C++ object stored in the block.
     */
    T             *object() {
        return (T *)storage;
    }
};

/**
 * Block that holds a bound C++ object (found by subtracting the offset of
 * the storage from the object's address).
 */
template<class T>
inline Instance<T> *instanceOf(T *object) {
    char *address = (char *)object - offsetof(Instance<T>, storage);
    return (Instance<T> *)address;
}

// Names of the bound functions ("UART.write"), indexed by the 'magic' value
// given to QuickJS, for error messages. Each context registers its classes
// and modules again: a name already stored keeps its index, so the table only
// holds the distinct names of the program and never grows past them. Per
// thread like the rest of the state (a runtime is set up and run on one
// thread); only pointers are thread_local (on the ESP every task gets a copy
// of the thread_local variables). The names are never freed.
inline thread_local const char **names = nullptr;  // array of nameCapacity names; grows
inline thread_local int          nameCount = 0;     // names stored
inline thread_local int          nameCapacity = 0;  // entries allocated in 'names'

// QuickJS keeps the 'magic' value of a function in an int16_t.
constexpr int maximumNames = 32768;

/**
 * Index of "owner.member" (or "member") in 'names', stored on first use: the
 * function's 'magic' value for QuickJS (0 when the table is full).
 */
inline int registerName(const char *owner, const char *member) {
    // build the name
    size_t size = strlen(member) + (owner != nullptr ? strlen(owner) + 1 : 0) + 1;
    char *name = (char *)memory::allocate(size);
    if (owner != nullptr) {
        snprintf(name, size, "%s.%s", owner, member);
    } else {
        snprintf(name, size, "%s", member);
    }

    // already registered (by an earlier context): keep its index
    for (int i = 0; i < nameCount; i++) {
        if (strcmp(names[i], name) == 0) {
            memory::release(name);
            return i;
        }
    }

    // table full: index 0, whose name will show in error messages
    if (nameCount >= maximumNames) {
        memory::release(name);
        return 0;
    }

    // make room (the capacity doubles), then store it at the next index
    if (nameCount == nameCapacity) {
        nameCapacity = nameCapacity == 0 ? 64 : nameCapacity * 2;
        names = (const char **)memory::reallocate(names, nameCapacity * sizeof(names[0]));
    }

    names[nameCount] = name;
    return nameCount++;
}

// ### Signatures of members and functions #####################################

template<class M> struct MemberTraits;

template<class T, class R, class... A>
struct MemberTraits<R (T::*)(A...)> {
    using Class = T;
    using Result = R;
    using Arguments = meta::TypeList<A...>;
    static constexpr bool isMethod = true;
};
template<class T, class R, class... A>
struct MemberTraits<R (T::*)(A...) const> : MemberTraits<R (T::*)(A...)> {};
template<class T, class R, class... A>
struct MemberTraits<R (T::*)(A...) noexcept> : MemberTraits<R (T::*)(A...)> {};
template<class T, class R, class... A>
struct MemberTraits<R (T::*)(A...) const noexcept> : MemberTraits<R (T::*)(A...)> {};

template<class T, class F>
struct MemberTraits<F T::*> {
    using Class = T;
    using Field = F;
    static constexpr bool isMethod = false;  // a data member, not a method
};

template<class F> struct FunctionTraits;
template<class R, class... A>
struct FunctionTraits<R (*)(A...)> {
    using Result = R;
    using Arguments = meta::TypeList<A...>;
};

/**
 * Number of JS arguments (js::Args does not count).
 */
template<class List> struct JsArity;
template<class... A>
struct JsArity<meta::TypeList<A...>> {
    static constexpr int value = (0 + ... + (meta::IsSame<meta::Decay<A>, Args>::value ? 0 : 1));
};

// #############################################################################
// Converted arguments, stored as the bases of ArgPack (one per index).

template<size_t I, class A>
struct ArgSlot {
    meta::Decay<A> value{};  // argument I, converted from JS (zeroed until then)
};

template<class Indices, class... A> struct ArgPack;

template<size_t... I, class... A>
struct ArgPack<meta::Indices<I...>, A...> : ArgSlot<I, A>... {
    /**
     * Converts the JS arguments into the slots, in order; stops at the first
     * failure (the error is then recorded in the current call).
     */
    bool convert(JSContext *ctx, int argc, JSValueConst *argv, const char *function) {
        // a function without arguments folds to true without using them
        (void)ctx;
        (void)argc;
        (void)argv;
        (void)function;
        return (convertOne<I, A>(ctx, argc, argv, function) && ...);
    }

    /**
     * Converts argument J (missing arguments read as undefined). A js::Args
     * parameter receives all the arguments from J on, unconverted.
     */
    template<size_t J, class B>
    bool convertOne(JSContext *ctx, int argc, JSValueConst *argv, const char *function) {
        // the slot of argument J
        meta::Decay<B> &target = ((ArgSlot<J, B> &)*this).value;

        if constexpr (meta::IsSame<meta::Decay<B>, Args>::value) {
            // js::Args: the arguments from J on, as they are
            int first = (int)J;
            target = Args{ ctx, argc > first ? argc - first : 0, argc > first ? argv + first : argv };
            return true;
        } else {
            // any other type: convert argument J (undefined if missing); an
            // error gives its 1-based number
            Where where = { function, (int)J + 1, nullptr };
            JSValueConst value = (int)J < argc ? argv[J] : JS_UNDEFINED;
            return Convert<meta::Decay<B>>::from(ctx, value, target, where);
        }
    }

    /**
     * Converted argument J.
     */
    template<size_t J, class B>
    meta::Decay<B> &get() {
        return ((ArgSlot<J, B> &)*this).value;
    }
};

/**
 * Converts the result of a call, unless the call failed.
 */
template<class R>
inline JSValue convertResult(JSContext *ctx, R &&result) {
    // an error recorded during the call wins over the result
    if (currentCall->failed) {
        return JS_EXCEPTION;
    }

    return Convert<meta::Decay<R>>::to(ctx, result);
}

/**
 * Calls 'object->*M' with the JS arguments; a CallScope must be active.
 * JS_EXCEPTION if a conversion or the call failed.
 */
template<auto M, class T, size_t... I, class... A>
inline JSValue callMethod(JSContext *ctx, T *object, int argc, JSValueConst *argv, const char *function,
                          meta::Indices<I...>, meta::TypeList<A...>) {
    // convert the JS arguments into a pack
    using Result = typename MemberTraits<decltype(M)>::Result;
    ArgPack<meta::Indices<I...>, A...> arguments;
    if (!arguments.convert(ctx, argc, argv, function)) {
        return JS_EXCEPTION;
    }

    // call, passing them as rvalues (a parameter taken by value is moved
    // from the pack), then convert the result unless the call failed
    if constexpr (meta::IsSame<Result, void>::value) {
        (object->*M)((A &&)(arguments.template get<I, A>())...);
        return currentCall->failed ? JS_EXCEPTION : JS_UNDEFINED;
    } else {
        return convertResult(ctx, (object->*M)((A &&)(arguments.template get<I, A>())...));
    }
}

/**
 * Same, deducing the parameter types and their indices from the method.
 */
template<auto M, class T>
inline JSValue callMethod(JSContext *ctx, T *object, int argc, JSValueConst *argv, const char *function) {
    using Arguments = typename MemberTraits<decltype(M)>::Arguments;
    return callMethod<M>(ctx, object, argc, argv, function, meta::MakeIndices<Arguments::size>(), Arguments());
}

/**
 * Calls the free function F with the JS arguments; a CallScope must be
 * active. JS_EXCEPTION if a conversion or the call failed.
 */
template<auto F, size_t... I, class... A>
inline JSValue callFunction(JSContext *ctx, int argc, JSValueConst *argv, const char *function,
                            meta::Indices<I...>, meta::TypeList<A...>) {
    // convert the JS arguments into a pack
    using Result = typename FunctionTraits<decltype(F)>::Result;
    ArgPack<meta::Indices<I...>, A...> arguments;
    if (!arguments.convert(ctx, argc, argv, function)) {
        return JS_EXCEPTION;
    }

    // call, passing them as rvalues, then convert the result unless the call
    // failed
    if constexpr (meta::IsSame<Result, void>::value) {
        F((A &&)(arguments.template get<I, A>())...);
        return currentCall->failed ? JS_EXCEPTION : JS_UNDEFINED;
    } else {
        return convertResult(ctx, F((A &&)(arguments.template get<I, A>())...));
    }
}

/**
 * Entry point from JS for the free function F ('magic': index of its name in
 * 'names').
 */
template<auto F>
inline JSValue functionTrampoline(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int magic) {
    // open the scope of the call, then convert, call and convert back
    CallScope scope(ctx);
    using Arguments = typename FunctionTraits<decltype(F)>::Arguments;
    return callFunction<F>(ctx, argc, argv, names[magic], meta::MakeIndices<Arguments::size>(), Arguments());
}

// #############################################################################
// Objects kept alive while they have pending activity (see js::keepAlive).
//
// Keeping takes a counted reference on the JS object and links its header
// into the 'kept' list. Releasing unlinks it and moves it to the 'released'
// list; the reference itself is freed later by collectReleased(), at a safe
// point. Keeping again before that reuses the reference still held.

inline thread_local InstanceHeader *firstKept = nullptr;
inline thread_local int keptCount = 0;
// Released by keepAlive(false); freed later, at a safe point.
inline thread_local InstanceHeader *firstReleased = nullptr;

template<class T, class = void> struct ClassAccess;
template<class T> class ClassBuilder;

template<class T, class = void> struct IsBoundClass { static constexpr bool value = false; };
template<class T>
struct IsBoundClass<T, meta::Void<decltype(jsIsBoundClass((T *)nullptr))>> {
    static constexpr bool value = true;
};

} // namespace detail

/**
 * Keeps the JS object of 'object' alive (and the event loop running) while it
 * has pending activity: open port, pending read... False when it ends.
 */
template<class T>
inline void keepAlive(T *object, bool keep) {
    // already in the requested state: nothing to do
    detail::InstanceHeader *header = &detail::instanceOf(object)->header;
    if (keep == header->keptAlive) {
        return;
    }

    // keep: hold a counted reference and link into the 'kept' list
    header->keptAlive = keep;
    if (keep) {
        if (header->releasePending) {
            // kept again before the deferred release: take the header out of
            // the 'released' list and reuse the reference still held
            detail::InstanceHeader **link = &detail::firstReleased;

            while (*link != header) {
                link = &(*link)->nextKept;
            }

            *link = header->nextKept;
            header->releasePending = false;
        } else {
            // otherwise take a new reference on the JS object
            header->keepReference = JS_DupValueRT(header->rt, header->self);
        }

        // push the header at the front of the 'kept' list
        header->previousKept = nullptr;
        header->nextKept = detail::firstKept;
        if (detail::firstKept != nullptr) {
            detail::firstKept->previousKept = header;
        }

        detail::firstKept = header;
        detail::keptCount++;
        return;
    }

    // release: unlink the header from the 'kept' list
    if (header->previousKept != nullptr) {
        header->previousKept->nextKept = header->nextKept;
    } else {
        detail::firstKept = header->nextKept;
    }

    if (header->nextKept != nullptr) {
        header->nextKept->previousKept = header->previousKept;
    }

    detail::keptCount--;

    // move it to the 'released' list: freeing the reference now could destroy
    // the object while one of its methods is running, collectReleased() frees
    // it later
    header->releasePending = true;
    header->nextKept = detail::firstReleased;
    detail::firstReleased = header;
}

/**
 * Number of objects kept alive (the event loop runs while it is > 0).
 */
inline int keptAliveCount() {
    return detail::keptCount;
}

/**
 * Frees the references released by keepAlive(false). Called by the event
 * loop between two dispatches.
 */
inline void collectReleased() {
    while (detail::firstReleased != nullptr) {
        // pop the first released header
        detail::InstanceHeader *header = detail::firstReleased;
        detail::firstReleased = header->nextKept;
        header->releasePending = false;

        // free its reference, taken out of the header first: freeing it may
        // finalize the object, header included
        JSValue reference = header->keepReference;
        header->keepReference = JS_UNDEFINED;
        JS_FreeValueRT(header->rt, reference);
    }
}

/**
 * Releases every object kept alive and frees the references (before
 * JS_FreeRuntime).
 */
inline void releaseAllKeptAlive() {
    // move every kept object to the 'released' list
    while (detail::firstKept != nullptr) {
        detail::InstanceHeader *header = detail::firstKept;
        detail::firstKept = header->nextKept;
        header->keptAlive = false;
        header->releasePending = true;
        header->nextKept = detail::firstReleased;
        detail::firstReleased = header;
    }

    // then free all the references
    detail::keptCount = 0;
    collectReleased();
}

/**
 * A C++ object created from C++ (not by 'new' in JS), with its JS object.
 */
template<class T>
struct Created {
    T    *object;     // nullptr if the creation failed
    Value value;   // the JS object
};

/**
 * Creates a bound object from C++ (defined below).
 */
template<class T> Created<T> create(JSContext *ctx = nullptr);

namespace detail {

/**
 * Defines the members of a class on its prototype.
 */
template<class T>
class ClassBuilder {
public:
    /**
     * Builder for 'prototype' (borrowed: the caller keeps it alive).
     */
    ClassBuilder(JSContext *ctx, JSValueConst prototype) : _ctx(ctx), _prototype(prototype), _ok(true) {}

    /**
     * False if a member could not be added (its exception is pending).
     */
    bool ok() const {
        return _ok;
    }

    /**
     * Adds member M to the prototype under 'name' (called by the jsRegister
     * function that JS_EXPORT declares). Does nothing once a member failed.
     */
    template<auto M>
    void add(const char *name) {
        // an earlier member failed: its exception stays the pending one
        if (!_ok) {
            return;
        }

        // register "Class.member": its index is the 'magic' value of the
        // trampolines, for error messages
        using Traits = MemberTraits<decltype(M)>;
        int nameIndex = registerName(T::jsClassName, name);

        if constexpr (Traits::isMethod) {
            // a method: a function property calling methodTrampoline<M>
            constexpr int arity = JsArity<typename Traits::Arguments>::value;
            JSValue function = JS_NewCFunctionMagic(_ctx, &methodTrampoline<M>, name, arity,
                                                    JS_CFUNC_generic_magic, nameIndex);
            if (JS_IsException(function)) {
                _ok = false;
                return;
            }

            // (the property takes the function, even on failure)
            int defined = JS_DefinePropertyValueStr(_ctx, _prototype, name, function,
                                                    JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
            _ok = defined >= 0;
        } else {
            // a field: a getter, plus a setter unless the field is const
            using Field = typename Traits::Field;
            JSValue getter = JS_NewCFunctionMagic(_ctx, &getterTrampoline<M>, name, 0,
                                                  JS_CFUNC_generic_magic, nameIndex);
            JSValue setter = JS_UNDEFINED;
            if constexpr (!meta::IsConst<Field>::value) {
                setter = JS_NewCFunctionMagic(_ctx, &setterTrampoline<M>, name, 1,
                                              JS_CFUNC_generic_magic, nameIndex);
            }

            // either function or the property name could not be created:
            // free what was
            JSAtom atom = JS_NewAtom(_ctx, name);
            if (JS_IsException(getter) || JS_IsException(setter) || atom == JS_ATOM_NULL) {
                JS_FreeValue(_ctx, getter);
                JS_FreeValue(_ctx, setter);
                JS_FreeAtom(_ctx, atom);
                _ok = false;
                return;
            }

            // defined as an accessor property of the prototype (which takes
            // the getter and the setter, even on failure)
            int defined = JS_DefinePropertyGetSet(_ctx, _prototype, atom, getter, setter, JS_PROP_CONFIGURABLE);
            JS_FreeAtom(_ctx, atom);
            _ok = defined >= 0;
        }
    }

private:
    /**
     * C++ object behind 'this'; nullptr (with a TypeError thrown by QuickJS)
     * if 'this' is not an object of class T.
     */
    static T *thisObject(JSContext *ctx, JSValueConst thisValue) {
        Instance<T> *instance = (Instance<T> *)JS_GetOpaque2(ctx, thisValue, ClassId<T>::id);
        return instance != nullptr ? instance->object() : nullptr;
    }

    /**
     * Entry point from JS for method M ('magic': index of its name in
     * 'names').
     */
    template<auto M>
    static JSValue methodTrampoline(JSContext *ctx, JSValueConst thisValue, int argc, JSValueConst *argv, int magic) {
        // open the scope of the call; 'this' must be an object of class T
        CallScope scope(ctx);
        T *object = thisObject(ctx, thisValue);
        if (object == nullptr) {
            return JS_EXCEPTION;
        }

        // convert the arguments, call, convert the result
        return callMethod<M>(ctx, object, argc, argv, names[magic]);
    }

    /**
     * Entry point from JS for reading field M.
     */
    template<auto M>
    static JSValue getterTrampoline(JSContext *ctx, JSValueConst thisValue, int, JSValueConst *, int) {
        // open the scope of the call; 'this' must be an object of class T
        CallScope scope(ctx);
        T *object = thisObject(ctx, thisValue);
        if (object == nullptr) {
            return JS_EXCEPTION;
        }

        // convert the field's current value
        using Field = meta::Decay<typename MemberTraits<decltype(M)>::Field>;
        return Convert<Field>::to(ctx, object->*M);
    }

    /**
     * Entry point from JS for writing field M; the field is unchanged if the
     * new value does not convert.
     */
    template<auto M>
    static JSValue setterTrampoline(JSContext *ctx, JSValueConst thisValue, int argc, JSValueConst *argv, int magic) {
        // open the scope of the call; 'this' must be an object of class T
        CallScope scope(ctx);
        T *object = thisObject(ctx, thisValue);
        if (object == nullptr) {
            return JS_EXCEPTION;
        }

        // convert the new value into a temporary; the error names the
        // property, with no argument number
        using Field = meta::Decay<typename MemberTraits<decltype(M)>::Field>;
        Field value;
        Where where = { names[magic], 0, nullptr };
        if (!Convert<Field>::from(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, value, where)) {
            return JS_EXCEPTION;
        }

        // then move it into the field
        object->*M = meta::move(value);
        return JS_UNDEFINED;
    }

    JSContext   *_ctx;        // context of the class registration
    JSValueConst _prototype;  // prototype receiving the members; borrowed
    bool         _ok;         // false once a member could not be added
};

/**
 * Friend of every bound class (declared by JS_CLASS): registration,
 * construction, finalization, GC marking.
 */
template<class T, class>
struct ClassAccess {
    /**
     * JS name of the class (the members declared by JS_CLASS are private:
     * only this friend reads them).
     */
    static const char    *name() {
        return T::jsClassName;
    }

    // value: true if a JS_EXPORT uses slot I
    template<int I, class = void> struct HasSlot { static constexpr bool value = false; };
    template<int I>
    struct HasSlot<I, meta::Void<decltype(T::jsRegister(Slot<I>(), meta::declval<ClassBuilder<T> &>()))>> {
        static constexpr bool value = true;
    };

    // Optional 'init' method: the JS constructor arguments.
    template<class U, class = void> struct HasInitImpl { static constexpr bool value = false; };
    template<class U>
    struct HasInitImpl<U, meta::Void<decltype(&U::init)>> { static constexpr bool value = true; };
    static constexpr bool hasInit = HasInitImpl<T>::value;  // T has an 'init' method

    /**
     * Calls the jsRegister overloads (one per JS_EXPORT) of the slots
     * I..maximumSlot that exist, in slot order.
     */
    template<int I>
    static void registerSlots(ClassBuilder<T> &builder) {
        // this slot, if a JS_EXPORT declared it
        if constexpr (HasSlot<I>::value) {
            T::jsRegister(Slot<I>(), builder);
        }

        // then the next ones (the recursion is unrolled at compile time)
        if constexpr (I < maximumSlot) {
            registerSlots<I + 1>(builder);
        }
    }

    /**
     * QuickJS finalizer: destroys the C++ object and frees its block.
     */
    static void finalize(JSRuntime *rt, JSValue value) {
        // no block: the creation failed before the C++ object was built
        Instance<T> *instance = (Instance<T> *)JS_GetOpaque(value, ClassId<T>::id);
        if (instance == nullptr) {
            return;
        }

        // destroy the C++ object with 'finalizing' set, so that calls into
        // JS are refused; then free the block
        finalizing++;
        instance->object()->~T();
        finalizing--;
        js_free_rt(rt, instance);
    }

    /**
     * QuickJS GC mark function: reports the js::Value members of the object
     * (registered while it was constructed).
     */
    static void mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *markFunction) {
        // no block: no C++ object, no member
        Instance<T> *instance = (Instance<T> *)JS_GetOpaque(value, ClassId<T>::id);
        if (instance == nullptr) {
            return;
        }

        // report each js::Value member
        for (Value *member = instance->header.firstMember; member != nullptr; member = member->nextMember()) {
            member->mark(rt, markFunction);
        }
    }

    /**
     * Registers the class in the runtime (once per runtime).
     */
    static void ensureClass(JSContext *ctx) {
        // one class id per C++ class, shared by all runtimes
        JSRuntime *rt = JS_GetRuntime(ctx);
        if (ClassId<T>::id == 0) {
            JS_NewClassID(&ClassId<T>::id);
        }

        // the class itself is registered in each runtime
        if (!JS_IsRegisteredClass(rt, ClassId<T>::id)) {
            static JSClassDef definition = { T::jsClassName, &finalize, &mark, nullptr, nullptr };
            JS_NewClass(rt, ClassId<T>::id, &definition);
        }
    }

    /**
     * Prototype of the class in this context, created on first use (the
     * exported members are added then). Returns a new reference, or
     * JS_EXCEPTION with the exception pending (nothing stored: the next use
     * tries again).
     */
    static JSValue prototype(JSContext *ctx) {
        // already created in this context: reuse it
        ensureClass(ctx);
        JSValue existing = JS_GetClassProto(ctx, ClassId<T>::id);
        if (JS_IsObject(existing)) {
            return existing;
        }

        // else create it
        JS_FreeValue(ctx, existing);
        JSValue created = JS_NewObject(ctx);
        if (JS_IsException(created)) {
            return JS_EXCEPTION;
        }

        // add the exported members
        ClassBuilder<T> builder(ctx, created);
        registerSlots<1>(builder);
        if (!builder.ok()) {
            JS_FreeValue(ctx, created);
            return JS_EXCEPTION;
        }

        // store it as the class prototype (which keeps its own reference)
        JS_SetClassProto(ctx, ClassId<T>::id, JS_DupValue(ctx, created));
        return created;
    }

    /**
     * Builds the C++ object (without arguments) in a new block attached to
     * 'object', whose finalizer destroys it from then on. nullptr on failure.
     */
    static Instance<T> *createInstance(JSContext *ctx, JSValueConst object) {
        // allocate the block with the QuickJS allocator
        Instance<T> *instance = (Instance<T> *)js_mallocz(ctx, sizeof(Instance<T>));
        if (instance == nullptr) {
            markPendingException();
            return nullptr;
        }

        // fill the header: no member yet, not kept alive; 'self' is not
        // counted (the JS object owns the block)
        instance->header.rt = JS_GetRuntime(ctx);
        instance->header.self = object;
        instance->header.firstMember = nullptr;
        instance->header.keptAlive = false;
        instance->header.releasePending = false;
        instance->header.keepReference = JS_UNDEFINED;

        // construct T() while 'constructing' covers its storage: each
        // js::Value member links itself into the header (see Value::attach)
        Constructing constructing = {
            (const char *)instance->storage,
            (const char *)instance->storage + sizeof(T),
            &instance->header,
        };
        Constructing *previous = detail::constructing;
        detail::constructing = &constructing;
        new (PlacementTag(), instance->storage) T();
        detail::constructing = previous;

        // attach the block to the JS object: its finalizer destroys it
        JS_SetOpaque(object, instance);
        return instance;
    }

    /**
     * 'new T(...)' in JS: creates the JS object and its C++ object, then calls
     * init() with the constructor arguments if T has one.
     */
    static JSValue construct(JSContext *ctx, JSValueConst newTarget, int argc, JSValueConst *argv, int magic) {
        // open the scope of the call; the prototype is the one of
        // 'new.target', so that JS subclasses work
        CallScope scope(ctx);
        JSValue prototypeValue = JS_GetPropertyStr(ctx, newTarget, "prototype");
        if (JS_IsException(prototypeValue)) {
            return JS_EXCEPTION;
        }

        // create the JS object of class T
        JSValue object = JS_NewObjectProtoClass(ctx, prototypeValue, ClassId<T>::id);
        JS_FreeValue(ctx, prototypeValue);
        if (JS_IsException(object)) {
            return JS_EXCEPTION;
        }

        // build the C++ object in it (T() may report an error too); on
        // failure, freeing the JS object frees everything
        Instance<T> *instance = createInstance(ctx, object);
        if (instance == nullptr || scope.failed) {
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }

        // call init() with the constructor arguments, if T has one; on
        // failure the finalizer of the freed JS object destroys the C++ object
        if constexpr (hasInit) {
            JSValue result = callMethod<&T::init>(ctx, instance->object(), argc, argv, names[magic]);
            if (JS_IsException(result)) {
                JS_FreeValue(ctx, object);
                return JS_EXCEPTION;
            }

            JS_FreeValue(ctx, result);
        }

        return object;
    }

    /**
     * Number of JS arguments of the constructor ('length' of the class):
     * those of init(), 0 without init().
     */
    static constexpr int initArity() {
        if constexpr (hasInit) {
            return JsArity<typename MemberTraits<decltype(&T::init)>::Arguments>::value;
        } else {
            return 0;
        }
    }

    /**
     * The JS constructor of the class (prototype created if needed), or
     * JS_EXCEPTION with the exception pending.
     */
    static JSValue constructor(JSContext *ctx) {
        // the prototype
        JSValue proto = prototype(ctx);
        if (JS_IsException(proto)) {
            return JS_EXCEPTION;
        }

        // a constructor named after the class, whose 'length' is the arity of
        // init(), linked both ways with the prototype
        int nameIndex = registerName(nullptr, T::jsClassName);
        JSValue function = JS_NewCFunctionMagic(ctx, &construct, T::jsClassName, initArity(),
                                                JS_CFUNC_constructor_magic, nameIndex);
        if (!JS_IsException(function)) {
            JS_SetConstructor(ctx, function, proto);
        }

        JS_FreeValue(ctx, proto);
        return function;
    }

    /**
     * Creates a JS object of class T and its C++ object (constructed without
     * arguments) for js::create(); a CallScope must be active. '*object' gets
     * the JS object (undefined if it could not be made), to free on failure.
     * nullptr if anything failed, the error recorded in the current call.
     */
    static T *createObject(JSContext *ctx, JSValue *object) {
        // a JS object of class T with the class prototype
        *object = JS_UNDEFINED;
        JSValue proto = prototype(ctx);
        if (JS_IsException(proto)) {
            markPendingException();
            return nullptr;
        }

        JSValue created = JS_NewObjectProtoClass(ctx, proto, ClassId<T>::id);
        JS_FreeValue(ctx, proto);
        if (JS_IsException(created)) {
            markPendingException();
            return nullptr;
        }

        // its C++ object; T() may report an error with js::fail
        *object = created;
        Instance<T> *instance = createInstance(ctx, created);
        if (instance == nullptr || currentCall->failed) {
            return nullptr;
        }

        return instance->object();
    }
};

} // namespace detail

/**
 * Creates a bound object from C++ like 'new T()', but without calling init(),
 * in 'ctx' or the current call's. On failure 'object' is nullptr and the
 * error goes to the current call, or is printed outside of any call (also
 * when there is no context at all). Nothing is created once the current call
 * has failed.
 */
template<class T>
Created<T> create(JSContext *ctx) {
    // no context given and no current call: nowhere to create the object
    JSContext *target = ctx != nullptr ? ctx : js::context();
    if (target == nullptr) {
        fail("js::create<%s>: no context (outside of a JS call, pass one)", detail::ClassAccess<T>::name());
        return Created<T>{ nullptr, Value() };
    }

    // the first error wins: once the current call failed, nothing is created
    if (failed()) {
        return Created<T>{ nullptr, Value() };
    }

    // create it inside a scope of its own, which catches every error, a
    // js::fail of the constructor T() included
    JSValue object = JS_UNDEFINED;
    T *created = nullptr;
    bool creationFailed = false;
    {
        detail::CallScope scope(target);
        created = detail::ClassAccess<T>::createObject(target, &object);
        creationFailed = scope.failed;
    }

    // failed: freeing the JS object frees everything (its finalizer destroys
    // the C++ object); the exception goes to the enclosing call, or is printed
    if (creationFailed) {
        JS_FreeValue(target, object);
        detail::propagateException(target);
        return Created<T>{ nullptr, Value() };
    }

    return Created<T>{ created, Value(target, object) };
}

/**
 * Pointers to bound classes: an argument 'Uart *uart' accepts a UART object.
 */
template<class T>
struct Convert<T *, typename meta::EnableIf<detail::IsBoundClass<T>::value>::Type> {
    /**
     * Accepts only a JS object of class T (not null): 'out' points to its
     * C++ object, which JS keeps owning.
     */
    static bool from(JSContext *, JSValueConst value, T *&out, const detail::Where &where) {
        // JS_GetOpaque checks the class: anything else is an error naming it
        void *opaque = JS_GetOpaque(value, detail::ClassId<T>::id);
        if (opaque == nullptr) {
            char expectation[128];
            snprintf(expectation, sizeof(expectation), "must be a %s", detail::ClassAccess<T>::name());
            return detail::conversionError(where, expectation);
        }

        out = ((detail::Instance<T> *)opaque)->object();
        return true;
    }

    /**
     * New reference on the JS object of a bound C++ object; nullptr gives
     * null.
     */
    static JSValue to(JSContext *ctx, T *value) {
        if (value == nullptr) {
            return JS_NULL;
        }

        return JS_DupValue(ctx, detail::instanceOf(value)->header.self);
    }
};

} // namespace js

/**
 * Marks a C++ class as bound to JS (first line of the class body): makes
 * ClassAccess and ClassBuilder friends, declares jsIsBoundClass (detected by
 * Convert<T *>), the JS class name and the __COUNTER__ base of the JS_EXPORT
 * slots. The members it declares are private.
 */
#define JS_CLASS(Name)                                                              \
    template<class, class> friend struct ::js::detail::ClassAccess;                 \
    template<class> friend class ::js::detail::ClassBuilder;                        \
    friend constexpr bool jsIsBoundClass(Name *) { return true; }                   \
    using JsSelf = Name;                                                            \
    static constexpr const char *jsClassName = #Name;                               \
    static constexpr int jsCounterBase = __COUNTER__

/**
 * Exposes 'member' (method or field, usually declared right after the
 * marker) to JS under the same name: declares jsRegister for the next slot,
 * whose body adds the member to the prototype. A slot past
 * detail::maximumSlot (64) fails to compile (see detail::Slot).
 */
#define JS_EXPORT(member)                                                           \
    static void jsRegister(::js::detail::Slot<__COUNTER__ - jsCounterBase>,         \
                           ::js::detail::ClassBuilder<JsSelf> &jsBuilder)           \
    {                                                                               \
        jsBuilder.template add<&JsSelf::member>(#member);                           \
    }
