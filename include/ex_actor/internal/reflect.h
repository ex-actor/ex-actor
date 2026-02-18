// Copyright 2025 The ex_actor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>

#include <exec/task.hpp>
#include <rfl/Tuple.hpp>
#include <stdexec/execution.hpp>

namespace ex_actor::internal {

template <typename T>
struct Signature;

template <class R, class... Args>
struct Signature<R (*)(Args...)> {
  using ReturnType = R;
  using ArgsTupleType = std::tuple<Args...>;
  using DecayedArgsTupleType = std::tuple<std::decay_t<Args>...>;
};

template <typename R, typename C, typename... Args>
struct Signature<R (C::*)(Args...)> {
  using ClassType = C;
  using ReturnType = R;
  using ArgsTupleType = std::tuple<Args...>;
  using DecayedArgsTupleType = std::tuple<std::decay_t<Args>...>;
};
template <typename R, typename C, typename... Args>
struct Signature<R (C::*)(Args...) const> : Signature<R (C::*)(Args...)> {};
template <typename R, typename C, typename... Args>
struct Signature<R (C::* const)(Args...)> : Signature<R (C::*)(Args...)> {};
template <typename R, typename C, typename... Args>
struct Signature<R (C::* const)(Args...) const> : Signature<R (C::*)(Args...)> {};

template <class This, template <class...> class Other>
constexpr bool kIsSpecializationOf = false;

template <template <class...> class Template, class... Args>
constexpr bool kIsSpecializationOf<Template<Args...>, Template> = true;

template <class This, template <class...> class Other>
concept SpecializationOf = kIsSpecializationOf<std::decay_t<This>, Other>;

template <auto kF1, auto kF2>
constexpr bool IsSameMemberFn() {
  if constexpr (!std::is_same_v<decltype(kF1), decltype(kF2)>) {
    return false;
  } else {
    return kF1 == kF2;
  }
}

template <class T>
struct ExTaskTraits;

template <class T>
struct ExTaskTraits<exec::task<T>> {
  using InnerType = T;
};

template <class R, class T>
concept RangeOf = std::ranges::range<R> && std::same_as<std::ranges::range_value_t<R>, T>;

template <typename T, typename... Ts>
consteval std::optional<size_t> GetIndexInParamPack() {
  if constexpr (sizeof...(Ts) != 0) {
    constexpr bool kMatches[] = {std::is_same_v<T, Ts>...};
    for (std::size_t i = 0; i < sizeof...(Ts); ++i) {
      if (kMatches[i]) return i;
    }
  }
  return std::nullopt;
}

template <size_t kIndex, class... Ts>
using ParamPackElement = std::tuple_element_t<kIndex, std::tuple<Ts...>>;

template <stdexec::sender Sender>
using CoAwaitType =
    decltype(std::declval<exec::task<void>::promise_type>().await_transform(std::declval<Sender>()).await_resume());

template <class Awaitable, class T>
concept AwaitableOf = std::is_same_v<CoAwaitType<Awaitable>, T>;

template <auto kMethod>
constexpr auto UnwrapReturnSenderIfNested() {
  using ReturnType = Signature<decltype(kMethod)>::ReturnType;
  constexpr bool kIsNested = stdexec::sender<ReturnType>;
  if constexpr (kIsNested) {
    return std::type_identity<CoAwaitType<ReturnType>> {};
  } else {
    return std::type_identity<ReturnType> {};
  }
}

template <auto kFunc>
std::string GetUniqueNameForFunction() {
  // TODO: according to the standard, typeid().name() is not guaranteed to be unique, but in modern gcc&clang,
  // it's an unique mangled name. So it works now, maybe replace it with a more robust way in the future.
  return typeid(kFunc).name();
}
}  // namespace ex_actor::internal
