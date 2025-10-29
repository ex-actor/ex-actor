#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>

#include <exec/task.hpp>
#include <rfl/Tuple.hpp>
#include <stdexec/execution.hpp>

namespace ex_actor::internal::reflect {

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

// ---------------- Actor Methods Related ----------------
#ifndef EXA_ACTOR_METHODS_KEYWORD
#define EXA_ACTOR_METHODS_KEYWORD kActorMethods
#endif

template <class T>
constexpr std::tuple EXA_ACTOR_METHODS_KEYWORD = {};

template <typename, typename = void>
struct HasStaticMemberActorMethods : std::false_type {};
template <typename T>
struct HasStaticMemberActorMethods<T, std::void_t<decltype(T::EXA_ACTOR_METHODS_KEYWORD)>> : std::true_type {};
template <typename T>
inline constexpr bool kHasStaticMemberActorMethods = HasStaticMemberActorMethods<T>::value;

template <class UserClass>
consteval auto GetActorMethodsTuple() {
  if constexpr (kHasStaticMemberActorMethods<UserClass>) {
    return UserClass::EXA_ACTOR_METHODS_KEYWORD;
  } else {
    return EXA_ACTOR_METHODS_KEYWORD<UserClass>;
  }
}

template <class T>
concept HasActorMethods = (std::tuple_size_v<decltype(GetActorMethodsTuple<T>())> > 0);

template <class UserClass>
consteval void CheckHasActorMethods() {
  static_assert(HasActorMethods<UserClass>,
                "Actor class must define constexpr static member `kActorMethods`, which is a std::tuple contains "
                "method pointers to your actor methods.");
}

template <auto kFunc, size_t kIndex = 0>
  requires std::is_member_function_pointer_v<decltype(kFunc)>
consteval std::optional<uint64_t> GetActorMethodIndex() {
  using ClassType = Signature<decltype(kFunc)>::ClassType;
  constexpr auto kActorMethodsTuple = GetActorMethodsTuple<ClassType>();
  if constexpr (std::tuple_size_v<decltype(kActorMethodsTuple)> == 0) {
    return std::nullopt;
  } else if constexpr (IsSameMemberFn<std::get<kIndex>(kActorMethodsTuple), kFunc>()) {
    return kIndex;
  } else if constexpr (kIndex + 1 < std::tuple_size_v<decltype(kActorMethodsTuple)>) {
    return GetActorMethodIndex<kFunc, kIndex + 1>();
  }
  return std::optional<uint64_t> {};
};

template <class UserClass, size_t kMethodIndex, class... Args>
auto InvokeActorMethod(UserClass& user_class_instance, Args&&... args) {
  CheckHasActorMethods<UserClass>();
  constexpr auto kMethodPtr = std::get<kMethodIndex>(GetActorMethodsTuple<UserClass>());
  return (user_class_instance.*kMethodPtr)(std::forward<Args>(args)...);
}
template <stdexec::sender Sender>
using CoAwaitType =
    decltype(std::declval<exec::task<void>::promise_type>().await_transform(std::declval<Sender>()).await_resume());

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
}  // namespace ex_actor::internal::reflect

namespace ex_actor::reflect {
using ::ex_actor::internal::reflect::EXA_ACTOR_METHODS_KEYWORD;
}  // namespace ex_actor::reflect
