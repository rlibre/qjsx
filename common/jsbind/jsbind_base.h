/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// jsbind - C++17 bindings for QuickJS, without the C++ standard library,
// without exceptions and without RTTI.
//
// This header holds the small language-level tools the library needs and that
// would normally come from <utility> and <type_traits>.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "quickjs.h"
#include "jsbind_foreach.h"

// Placement new with a private tag, so that <new> is not needed.
namespace js { namespace detail { struct PlacementTag {}; } }

/**
 * Placement new: 'new (PlacementTag(), where) T()' constructs a T at 'where'
 * (used to build a bound C++ object inside the memory of its JS object).
 */
inline void *operator new(size_t, js::detail::PlacementTag, void *where) noexcept {
    return where;
}

/**
 * Matching placement delete, required by the language; nothing to free.
 */
inline void operator delete(void *, js::detail::PlacementTag, void *) noexcept {}

namespace js {
namespace meta {

template<class T> struct RemoveReference { using Type = T; };
template<class T> struct RemoveReference<T &> { using Type = T; };
template<class T> struct RemoveReference<T &&> { using Type = T; };

template<class T> struct RemoveConst { using Type = T; };
template<class T> struct RemoveConst<const T> { using Type = T; };

/**
 * Plain type of a parameter: 'const Options &' -> 'Options'
 */
template<class T>
using Decay = typename RemoveConst<typename RemoveReference<T>::Type>::Type;

template<class A, class B> struct IsSame { static constexpr bool value = false; };
template<class A> struct IsSame<A, A> { static constexpr bool value = true; };

template<class T> struct IsConst { static constexpr bool value = false; };
template<class T> struct IsConst<const T> { static constexpr bool value = true; };

/**
 * Only usable in unevaluated contexts (decltype), like std::declval.
 */
template<class T> T &&declval() noexcept;

/**
 * Like std::move: casts 'value' to an rvalue reference so that its contents
 * can be moved instead of copied.
 */
template<class T>
constexpr typename RemoveReference<T>::Type &&move(T &&value) noexcept {
    return (typename RemoveReference<T>::Type &&)value;
}

/**
 * Compile-time list of indices 0..N-1, like std::index_sequence.
 */
template<size_t... I> struct Indices {};
template<size_t N, size_t... I> struct MakeIndicesImpl : MakeIndicesImpl<N - 1, N - 1, I...> {};
template<size_t... I> struct MakeIndicesImpl<0, I...> { using Type = Indices<I...>; };
template<size_t N> using MakeIndices = typename MakeIndicesImpl<N>::Type;

// Type list
template<class... T> struct TypeList { static constexpr size_t size = sizeof...(T); };

template<class... T> using Void = void;

// Like std::enable_if: EnableIf<condition>::Type exists only if condition.
template<bool Condition, class T = void> struct EnableIf {};
template<class T> struct EnableIf<true, T> { using Type = T; };

} // namespace meta

namespace detail {

// JS_EXPORT slots scanned at registration: 1..maximumSlot (see jsbind_class.h)
constexpr int maximumSlot = 64;

/**
 * Distinct type per exported member slot (see JS_EXPORT). JS_EXPORT defines a
 * function taking a Slot<N>, which instantiates it: a slot past maximumSlot,
 * whose member would never be registered, fails to compile here.
 */
template<int N>
struct Slot {
    static_assert(N >= 1 && N <= maximumSlot,
                  "JS_EXPORT: more than 64 exports (or __COUNTER__ uses) since JS_CLASS in this class");
};

} // namespace detail
} // namespace js
