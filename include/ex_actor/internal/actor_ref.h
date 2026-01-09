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
#include <utility>

#include "ex_actor/internal/actor.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/network.h"
#include "ex_actor/internal/reflect.h"
#include "ex_actor/internal/serialization.h"

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

  void SetLocalRuntimeInfo(uint32_t this_node_id, TypeErasedActor* actor, network::MessageBroker* message_broker) {
    this_node_id_ = this_node_id;
    type_erased_actor_ = actor;
    message_broker_ = message_broker;
  }

  friend rfl::Reflector<ActorRef<UserClass>>;

  /**
   * @brief Send message to an actor. Returns a coroutine carrying the result.
   * @note This method requires your args and return value can be serialized by reflect-cpp, if you met compile errors
   * like "Unsupported type", refer https://rfl.getml.com/concepts/custom_classes/ to add a serializer for it. Or if you
   * can confirm it's a local actor, use SendLocal() instead, which doesn't require your args to be serializable.
   * @note The returned coroutine is not copyable. please use `co_await std::move(coroutine)`.
   */
  template <auto kMethod, class... Args>
  [[nodiscard]] auto Send(Args... args) const {
    // Add a fallback inline_scheduler for it.
    return util::WrapSenderWithInlineScheduler(SendInternal<kMethod>(std::move(args)...));
  }

  /**
   * @brief Send message to a local actor. Doesn't require your args to be serializable.
   */
  template <auto kMethod, class... Args>
  [[nodiscard]] ex::sender auto SendLocal(Args... args) const {
    static_assert(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>,
                  "method is not invocable with the provided arguments");
    if (IsEmpty()) [[unlikely]] {
      throw std::runtime_error("Empty ActorRef, cannot call method on it.");
    }
    EXA_THROW_CHECK_EQ(node_id_, this_node_id_) << "Cannot call remote actor using SendLocal, use Send instead.";
    return type_erased_actor_->template CallActorMethod<kMethod>(/*unsafe_message_slot_index=*/std::nullopt,
                                                                 std::move(args)...);
  }

  struct UnsafeSendOperation {
    TypeErasedActor* type_erased_actor = nullptr;
    size_t unsafe_message_slot_index = 0;

    template <auto kMethod, class... Args>
    [[nodiscard]] ex::sender auto Send(Args... args) const {
      using Sig = reflect::Signature<decltype(kMethod)>;
      static_assert(!stdexec::sender<typename Sig::ReturnType>,
                    "only works for methods that returns non-sender(one-step methods)");
      return type_erased_actor->template CallActorMethod<kMethod>(
          /*unsafe_message_slot_index=*/unsafe_message_slot_index, std::move(args)...);
    }
  };

  UnsafeSendOperation UnsafeMessageSlot(size_t index) {
    EXA_THROW_CHECK(node_id_ == this_node_id_) << "Cannot use unsafe message slot on remote actor.";
    return UnsafeSendOperation {.type_erased_actor = type_erased_actor_, .unsafe_message_slot_index = index};
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

  template <auto kMethod, class... Args>
  [[nodiscard]] auto SendInternal(Args... args) const
      -> exec::task<typename decltype(reflect::UnwrapReturnSenderIfNested<kMethod>())::type> {
    static_assert(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>,
                  "method is not invocable with the provided arguments");
    if (IsEmpty()) [[unlikely]] {
      throw std::runtime_error("Empty ActorRef, cannot call method on it.");
    }
    if (node_id_ == this_node_id_) {
      co_return co_await SendLocal<kMethod>(std::move(args)...);
    }

    // remote call
    EXA_THROW_CHECK(message_broker_ != nullptr) << "Message broker not set";
    using Sig = reflect::Signature<decltype(kMethod)>;
    serde::ActorMethodCallArgs<typename Sig::DecayedArgsTupleType> method_call_args {
        .args_tuple = typename Sig::DecayedArgsTupleType(std::move(args)...)};
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
      auto res = serde::Deserialize<serde::ActorMethodReturnError>(reader.Current(), reader.RemainingSize());
      EXA_THROW << res.error;
    }
    if constexpr (std::is_void_v<UnwrappedType>) {
      co_return;
    } else {
      auto res =
          serde::Deserialize<serde::ActorMethodReturnValue<UnwrappedType>>(reader.Current(), reader.RemainingSize());
      co_return res.return_value;
    }
  }
};

}  // namespace ex_actor::internal

namespace ex_actor {
using internal::ActorRef;
}  // namespace ex_actor

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
