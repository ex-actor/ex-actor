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
#include <cstdint>
#include <type_traits>

#include "ex_actor/internal/actor.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/network.h"
#include "ex_actor/internal/reflect.h"
#include "ex_actor/internal/serialization.h"
#include "rfl/internal/has_reflector.hpp"

namespace ex_actor::internal {
template <class UserClass>
class ActorRef {
 public:
  ActorRef() : is_empty_(true) {}

  ActorRef(uint32_t this_node_id, uint32_t node_id, uint64_t actor_id, TypeErasedActor* actor,
           network::MessageBroker* message_broker)
      : is_empty_(false),
        this_node_id_(this_node_id),
        node_id_(node_id),
        actor_id_(actor_id),
        type_erased_actor_(actor),
        message_broker_(message_broker) {}

  friend bool operator==(const ActorRef& lhs, const ActorRef& rhs) {
    if (lhs.is_empty_ && rhs.is_empty_) {
      return true;
    }
    return lhs.node_id_ == rhs.node_id_ && lhs.actor_id_ == rhs.actor_id_;
  }

  friend rfl::Reflector<ActorRef<UserClass>>;

  /**
   * @brief Send message to an actor. Returns a coroutine carrying the result.
   * @note This method requires your args and return value can be serialized by reflect-cpp, if you met compile errors
   * like "Unsupported type", refer https://rfl.getml.com/concepts/custom_classes/ to add a serializer for it. Or if you
   * can confirm it's a local actor, use SendLocal() instead, which doesn't require your args to be serializable.
   * @note Dynamic memory allocation will happen due to the use of coroutine. If you can confirm it's a local actor,
   * consider using SendLocal() instead, which has better performance.
   * @note The returned coroutine is not copyable. please use `co_await std::move(coroutine)`.
   */
  template <auto kMethod, class... Args>
  [[nodiscard]] auto Send(Args&&... args) const
      -> exec::task<typename decltype(reflect::UnwrapReturnSenderIfNested<kMethod>())::type> {
    static_assert(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>,
                  "method is not invocable with the provided arguments");
    if (IsEmpty()) [[unlikely]] {
      throw std::runtime_error("Empty ActorRef, cannot call method on it.");
    }
    if (node_id_ == this_node_id_) {
      co_return co_await SendLocal<kMethod>(std::forward<Args>(args)...);
    }

    // remote call
    EXA_THROW_CHECK(message_broker_ != nullptr) << "Message broker not set";
    using Sig = reflect::Signature<decltype(kMethod)>;
    serde::ActorMethodCallArgs<typename Sig::DecayedArgsTupleType> method_call_args {
        .args_tuple = typename Sig::DecayedArgsTupleType(std::forward<Args>(args)...)};
    auto serialized_args = serde::Serialize(method_call_args);
    std::string handler_key = reflect::GetUniqueNameForFunction<kMethod>();
    serde::BufferWriter buffer_writer(network::ByteBufferType {sizeof(serde::NetworkRequestType) + sizeof(uint64_t) +
                                                               sizeof(handler_key.size()) + handler_key.size() +
                                                               sizeof(actor_id_) + serialized_args.size()});
    // protocol: [request_type][handler_key_len][handler_key][actor_id][ActorMethodCallArgs]
    buffer_writer.WritePrimitive(serde::NetworkRequestType::kActorMethodCallRequest);
    buffer_writer.WritePrimitive(handler_key.size());
    buffer_writer.CopyFrom(handler_key.data(), handler_key.size());
    buffer_writer.WritePrimitive(actor_id_);
    buffer_writer.CopyFrom(serialized_args.data(), serialized_args.size());

    using UnwrappedType = decltype(reflect::UnwrapReturnSenderIfNested<kMethod>())::type;
    network::ByteBufferType response_buffer =
        co_await message_broker_->SendRequest(node_id_, std::move(buffer_writer).MoveBufferOut());
    serde::BufferReader reader {std::move(response_buffer)};
    auto type = reader.NextPrimitive<serde::NetworkReplyType>();
    if (type == serde::NetworkReplyType::kActorMethodCallError) {
      auto res =
          serde::Deserialize<serde::ActorMethodReturnValue<std::string>>(reader.Current(), reader.RemainingSize());
      EXA_THROW << res.return_value;
    }
    if constexpr (std::is_void_v<UnwrappedType>) {
      co_return;
    } else {
      auto res =
          serde::Deserialize<serde::ActorMethodReturnValue<UnwrappedType>>(reader.Current(), reader.RemainingSize());
      co_return res.return_value;
    }
  }

  /**
   * @brief Send message to a local actor. Has better performance than the generic Send(). No heap allocation.
   */
  template <auto kMethod, class... Args>
  [[nodiscard]] ex::sender auto SendLocal(Args&&... args) const {
    static_assert(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>,
                  "method is not invocable with the provided arguments");
    if (IsEmpty()) [[unlikely]] {
      throw std::runtime_error("Empty ActorRef, cannot call method on it.");
    }
    EXA_THROW_CHECK_EQ(node_id_, this_node_id_) << "Cannot call remote actor using SendLocal, use Send instead.";
    return type_erased_actor_->template CallActorMethod<kMethod>(std::forward<Args>(args)...);
  }

  bool IsEmpty() const { return is_empty_; }

  uint32_t GetNodeId() const { return node_id_; }
  uint64_t GetActorId() const { return actor_id_; }

 private:
  bool is_empty_;
  uint32_t this_node_id_ = 0;
  uint32_t node_id_ = 0;
  uint64_t actor_id_ = 0;
  TypeErasedActor* type_erased_actor_ = nullptr;
  network::MessageBroker* message_broker_ = nullptr;
};

class ActorRefDeserializationContext {
 public:
  static ActorRefDeserializationContext& GetThreadLocalInstance() {
    static thread_local ActorRefDeserializationContext instance;
    return instance;
  }

  uint32_t this_node_id = 0;
  std::function<TypeErasedActor*(uint64_t)> actor_look_up_fn;
  network::MessageBroker* message_broker = nullptr;
};
}  // namespace ex_actor::internal

namespace ex_actor {
using internal::ActorRef;
}  // namespace ex_actor

namespace rfl {
template <typename U>
struct Reflector<ex_actor::internal::ActorRef<U>> {
  struct ReflType {
    bool is_valid {};
    uint32_t node_id {};
    uint64_t actor_id {};
  };

  static ex_actor::internal::ActorRef<U> to(const ReflType& rfl_type) noexcept {
    auto& info = ex_actor::internal::ActorRefDeserializationContext::GetThreadLocalInstance();

    ex_actor::internal::ActorRef<U> actor(info.this_node_id, rfl_type.node_id, rfl_type.actor_id,
                                          info.actor_look_up_fn(rfl_type.actor_id), info.message_broker);
    return actor;
  }

  static ReflType from(const ex_actor::internal::ActorRef<U>& actor_ref) {
    return {
        .is_valid = actor_ref.is_empty_,
        .node_id = actor_ref.node_id_,
        .actor_id = actor_ref.actor_id_,
    };
  }
};
}  // namespace rfl

namespace std {
template <class UserClass>
struct hash<ex_actor::ActorRef<UserClass>> {
  size_t operator()(const ex_actor::ActorRef<UserClass>& ref) const {
    if (ref.IsEmpty()) {
      return ex_actor::internal::kEmptyActorRefHashVal;
    }
    return std::hash<uint64_t>()(ref.GetActorId()) ^ std::hash<uint32_t>()(ref.GetNodeId());
  }
};
}  // namespace std
