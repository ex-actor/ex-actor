// Copyright 2026 The ex_actor Authors.
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

#include <cstdint>
#include <type_traits>
#include <utility>

#include "ex_actor/internal/actor.h"
#include "ex_actor/internal/constants.h"

namespace ex_actor::internal {

template <class UserClass>
class LocalActorRef {
 public:
  LocalActorRef() : is_empty_(true) {}

  LocalActorRef(uint64_t actor_id, TypeErasedActor* actor)
      : is_empty_(false), actor_id_(actor_id), type_erased_actor_(actor) {}

  friend bool operator==(const LocalActorRef& lhs, const LocalActorRef& rhs) {
    if (lhs.is_empty_ && rhs.is_empty_) {
      return true;
    }
    return lhs.actor_id_ == rhs.actor_id_;
  }

  template <class U>
  friend class LocalActorRef;

  template <class U>
  friend class ActorRef;

  // Converting constructor from LocalActorRef<U> where U* is convertible to UserClass*
  template <class Other>
    requires std::is_convertible_v<Other*, UserClass*>
  // NOLINTNEXTLINE(google-explicit-constructor) - implicit conversion is intentional for polymorphism support
  LocalActorRef(const LocalActorRef<Other>& other)
      : is_empty_(other.is_empty_), actor_id_(other.actor_id_), type_erased_actor_(other.type_erased_actor_) {}

  // Converting assignment operator - delegates to converting constructor
  template <class Other>
    requires std::is_convertible_v<Other*, UserClass*>
  LocalActorRef<UserClass>& operator=(const LocalActorRef<Other>& other) {
    *this = LocalActorRef<UserClass>(other);
    return *this;
  }

  /**
   * @brief Send message to a local actor. No heap allocation.
   */
  template <auto kMethod, class... Args>
  [[nodiscard]] ex::sender auto SendLocal(Args... args) const {
    static_assert(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>,
                  "method is not invocable with the provided arguments");
    EXA_THROW_CHECK(!IsEmpty()) << "Empty LocalActorRef, cannot call method on it.";
    EXA_THROW_CHECK(type_erased_actor_ != nullptr)
        << "Local actor instance not set, it's typically because you converted a remote ActorRef to LocalActorRef.";
    return type_erased_actor_->template CallActorMethod<kMethod>(std::move(args)...);
  }

  bool IsEmpty() const { return is_empty_; }
  uint64_t GetActorId() const { return actor_id_; }

 protected:
  bool is_empty_;
  uint64_t actor_id_ = 0;
  TypeErasedActor* type_erased_actor_ = nullptr;
};

}  // namespace ex_actor::internal

namespace ex_actor {
using internal::LocalActorRef;
}  // namespace ex_actor

namespace std {
template <class UserClass>
struct hash<ex_actor::LocalActorRef<UserClass>> {
  size_t operator()(const ex_actor::LocalActorRef<UserClass>& ref) const {
    if (ref.IsEmpty()) {
      return ex_actor::internal::kEmptyActorRefHashVal;
    }
    return std::hash<uint64_t>()(ref.GetActorId());
  }
};
}  // namespace std
