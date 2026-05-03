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
class BasicActorRef {
 public:
  BasicActorRef() = default;

  BasicActorRef(uint64_t actor_id, TypeErasedActor* actor)
      : is_empty_(false),
        actor_id_(actor_id),
        type_erased_actor_(actor),
        adjusted_ptr_(actor != nullptr ? static_cast<UserClass*>(actor->GetUserClassInstanceAddress()) : nullptr),
        actor_type_hash_(actor != nullptr ? actor->GetActorTypeHash() : FnvHash(GetTypeName<UserClass>())) {}

  BasicActorRef(uint64_t actor_id, TypeErasedActor* actor, uint64_t actor_type_hash)
      : is_empty_(false),
        actor_id_(actor_id),
        type_erased_actor_(actor),
        adjusted_ptr_(actor != nullptr ? static_cast<UserClass*>(actor->GetUserClassInstanceAddress()) : nullptr),
        actor_type_hash_(actor_type_hash) {}

  friend bool operator==(const BasicActorRef& lhs, const BasicActorRef& rhs) {
    if (lhs.is_empty_ != rhs.is_empty_) {
      return false;
    }
    if (lhs.is_empty_) {
      return true;
    }
    return lhs.actor_id_ == rhs.actor_id_ && lhs.actor_type_hash_ == rhs.actor_type_hash_;
  }

  template <class U>
  friend class BasicActorRef;

  template <class U>
  friend class ActorRef;

  // Converting constructor from BasicActorRef<U> where U* is convertible to UserClass*
  template <class Other>
    requires std::is_convertible_v<Other*, UserClass*>
  // NOLINTNEXTLINE(google-explicit-constructor) - implicit conversion is intentional for polymorphism support
  BasicActorRef(const BasicActorRef<Other>& other)
      : is_empty_(other.is_empty_),
        actor_id_(other.actor_id_),
        type_erased_actor_(other.type_erased_actor_),
        // static_cast between typed pointers applies a compile-time constant offset,
        // so this is correct even if the pointer is invalid (e.g. a remote address).
        adjusted_ptr_(static_cast<UserClass*>(other.adjusted_ptr_)),
        actor_type_hash_(other.actor_type_hash_) {}

  // Converting assignment operator - delegates to converting constructor
  template <class Other>
    requires std::is_convertible_v<Other*, UserClass*>
  BasicActorRef<UserClass>& operator=(const BasicActorRef<Other>& other) {
    is_empty_ = other.is_empty_;
    actor_id_ = other.actor_id_;
    type_erased_actor_ = other.type_erased_actor_;
    // static_cast between typed pointers applies a compile-time constant offset,
    // so this is correct even if the pointer is invalid (e.g. a remote address).
    adjusted_ptr_ = static_cast<UserClass*>(other.adjusted_ptr_);
    actor_type_hash_ = other.actor_type_hash_;
    return *this;
  }

  /**
   * @brief Send message to a local actor. No heap allocation.
   */
  template <auto kMethod, class... Args>
  [[nodiscard]] ex::sender auto SendLocal(Args... args) const
    requires(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>)
  {
    EXA_THROW_CHECK(!IsEmpty()) << "Empty BasicActorRef, cannot call method on it.";
    EXA_THROW_CHECK(type_erased_actor_ != nullptr) << "Underlying actor instance not set, it's typically because you "
                                                      "converted a remote ActorRef to BasicActorRef.";
    return CallActorMethodUseTuple<kMethod>(type_erased_actor_, adjusted_ptr_, std::make_tuple(std::move(args)...));
  }

  bool IsEmpty() const { return is_empty_; }
  uint64_t GetActorId() const { return actor_id_; }
  uint64_t GetActorTypeHash() const { return actor_type_hash_; }

  /**
   * @brief Get the number of pending messages in the actor's mailbox synchronously.
   * @return The number of pending messages if the actor is local, or 0 if it's remote or empty.
   */
  size_t GetPendingMessageCountLocal() const {
    if (IsEmpty() || type_erased_actor_ == nullptr) {
      return 0;
    }
    return type_erased_actor_->GetPendingMessageCount();
  }

 protected:
  bool is_empty_ = true;
  uint64_t actor_id_ = 0;
  TypeErasedActor* type_erased_actor_ = nullptr;
  UserClass* adjusted_ptr_ = nullptr;
  uint64_t actor_type_hash_ = 0;
};

}  // namespace ex_actor::internal

namespace ex_actor {
using internal::BasicActorRef;
}  // namespace ex_actor

namespace std {
template <class UserClass>
struct hash<ex_actor::BasicActorRef<UserClass>> {
  size_t operator()(const ex_actor::BasicActorRef<UserClass>& ref) const {
    if (ref.IsEmpty()) {
      return ex_actor::internal::kEmptyActorRefHashVal;
    }
    return std::hash<uint64_t>()(ref.GetActorId());
  }
};
}  // namespace std
