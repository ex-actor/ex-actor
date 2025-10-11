#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>

#include <exec/task.hpp>
#include <stdexec/execution.hpp>

namespace ex_actor::reflect {

template <class UserClass>
constexpr std::tuple kActorMethods = UserClass::kActorMethods;

}  // namespace ex_actor::reflect

namespace ex_actor::internal::reflect {

template <typename T>
struct Signature;

template <typename R, typename C, typename... Args>
struct Signature<R (C::*)(Args...)> {
  using ClassType = C;
  using ReturnType = R;
  using ArgsTupleType = std::tuple<Args...>;
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
  constexpr bool kMatches[] = {std::is_same_v<T, Ts>...};
  for (std::size_t i = 0; i < sizeof...(Ts); ++i) {
    if (kMatches[i]) return i;
  }
  return std::nullopt;
}

template <size_t kIndex, class... Ts>
using ParamPackElement = std::tuple_element_t<kIndex, std::tuple<Ts...>>;

// ---------------- Actor Methods Related ----------------
using ::ex_actor::reflect::kActorMethods;

template <class T>
concept HasActorMethods = (std::tuple_size_v<decltype(kActorMethods<T>)> > 0);

template <class UserClass>
consteval void CheckHasActorMethods() {
  static_assert(HasActorMethods<UserClass>,
                "Actor class must define constexpr static member `kActorMethods`, which is a std::tuple contains "
                "method pointers to your actor methods.");
}

template <auto kFunc, size_t kIndex = 0>
  requires std::is_member_function_pointer_v<decltype(kFunc)>
consteval size_t GetActorMethodIndex() {
  using ClassType = Signature<decltype(kFunc)>::ClassType;
  CheckHasActorMethods<ClassType>();
  if constexpr (IsSameMemberFn<std::get<kIndex>(kActorMethods<ClassType>), kFunc>()) {
    return kIndex;
  } else if constexpr (kIndex + 1 < std::tuple_size_v<decltype(kActorMethods<ClassType>)>) {
    return GetActorMethodIndex<kFunc, kIndex + 1>();
  }
};

template <class UserClass, size_t kMethodIndex, class... Args>
auto InvokeActorMethod(UserClass& user_class_instance, Args&&... args) {
  CheckHasActorMethods<UserClass>();
  constexpr auto kMethodPtr = std::get<kMethodIndex>(kActorMethods<UserClass>);
  return (user_class_instance.*kMethodPtr)(std::forward<Args>(args)...);
}

}  // namespace ex_actor::internal::reflect