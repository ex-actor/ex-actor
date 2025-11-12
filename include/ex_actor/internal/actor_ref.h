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
struct ActorRefRflType {
  bool is_valid {};
  uint32_t node_id {};
  uint64_t actor_id {};
  uint64_t class_index_in_roster {};
};

template <class UserClass>
class ActorRef {
 public:
  ActorRef() : is_empty_(false) {}

  ActorRef(uint32_t this_node_id, uint32_t node_id, uint64_t actor_id, uint64_t class_index_in_roster,
           TypeErasedActor* actor, network::MessageBroker* message_broker)
      : is_empty_(true),
        this_node_id_(this_node_id),
        node_id_(node_id),
        actor_id_(actor_id),
        class_index_in_roster_(class_index_in_roster),
        type_erased_actor_(actor),
        message_broker_(message_broker) {}

  /**
   * @brief Called after deserialization. Some fields will change after deserialization in another node, like
   * this_node_id and message_broker.
   */
  void SetLocalRuntimeInfo(uint32_t this_node_id, TypeErasedActor* actor, network::MessageBroker* message_broker) {
    this_node_id_ = this_node_id;
    type_erased_actor_ = actor;
    message_broker_ = message_broker;
  }

  // reflect-cpp adaption start
  using ReflectionType = ActorRefRflType;
  explicit ActorRef(ActorRefRflType rfl_type)
      : is_empty_(rfl_type.is_valid),
        node_id_(rfl_type.node_id),
        actor_id_(rfl_type.actor_id),
        class_index_in_roster_(rfl_type.class_index_in_roster) {}
  ReflectionType reflection() const {
    return {.is_valid = is_empty_,
            .node_id = node_id_,
            .actor_id = actor_id_,
            .class_index_in_roster = class_index_in_roster_};
  }
  // reflect-cpp adaption end

  friend bool operator==(const ActorRef& lhs, const ActorRef& rhs) {
    if (lhs.is_empty_ == false && rhs.is_empty_ == false) {
      return true;
    }
    return lhs.node_id_ == rhs.node_id_ && lhs.actor_id_ == rhs.actor_id_;
  }

  friend rfl::Reflector<ActorRef<UserClass>>;

  /**
   * @brief Send message to an actor. Returns a coroutine carrying the result. Dynamic memory allocation will happen due
   * to the use of coroutine. If you can confirm it's a local actor, consider using SendLocal() instead, which has
   * better performance.
   * @note If your method argument can't be automatically serialized by reflect-cpp, refer
   * https://rfl.getml.com/concepts/custom_classes/ to add serializer for it. Or if you can confirm it's a local actor,
   * you can use SendLocal() to bypass the serialization code path.
   * @note The returned coroutine is not copyable. please use `co_await std::move(coroutine)`.
   */
  template <auto kMethod, class... Args>
  [[nodiscard]] auto Send(Args&&... args) const
      -> exec::task<typename decltype(reflect::UnwrapReturnSenderIfNested<kMethod>())::type> {
    static_assert(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>,
                  "method is not invocable with the provided arguments");
    if (!IsEmpty()) [[unlikely]] {
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
    constexpr std::optional<uint64_t> optional_method_index = reflect::GetActorMethodIndex<kMethod>();
    EXA_THROW_CHECK(optional_method_index.has_value())
        << "Can't find method index in actor methods. Please put this method to your actor class's method tuple.";
    uint64_t method_index = optional_method_index.value();
    serde::BufferWriter buffer_writer(network::ByteBufferType {sizeof(serde::NetworkRequestType) +
                                                               sizeof(class_index_in_roster_) + sizeof(method_index) +
                                                               sizeof(actor_id_) + serialized_args.size()});
    // protocol: [request_type][class_index_in_roster][method_index][actor_id][ActorMethodCallArgs]
    buffer_writer.WritePrimitive(serde::NetworkRequestType::kActorMethodCallRequest);
    buffer_writer.WritePrimitive(class_index_in_roster_);
    buffer_writer.WritePrimitive(method_index);
    buffer_writer.WritePrimitive(actor_id_);
    buffer_writer.CopyFrom(serialized_args.data(), serialized_args.size());

    using UnwrappedType = decltype(reflect::UnwrapReturnSenderIfNested<kMethod>())::type;
    auto sender = message_broker_->SendRequest(node_id_, std::move(buffer_writer).MoveBufferOut()) |
                  ex::then([](network::ByteBufferType response_buffer) {
                    serde::BufferReader reader {std::move(response_buffer)};
                    auto type = reader.NextPrimitive<serde::NetworkReplyType>();
                    if (type == serde::NetworkReplyType::kActorMethodCallError) {
                      EXA_THROW << serde::Deserialize<serde::ActorMethodReturnValue<std::string>>(
                                       reader.Current(), reader.RemainingSize())
                                       .return_value;
                    }
                    if constexpr (std::is_void_v<UnwrappedType>) {
                      return;
                    } else {
                      return serde::Deserialize<serde::ActorMethodReturnValue<UnwrappedType>>(reader.Current(),
                                                                                              reader.RemainingSize())
                          .return_value;
                    }
                  });
    co_return co_await std::move(sender);
  }

  /**
   * @brief Send message to a local actor. Has better performance than the generic Send(). No heap allocation.
   */
  template <auto kMethod, class... Args>
  [[nodiscard]] ex::sender auto SendLocal(Args&&... args) const {
    static_assert(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>,
                  "method is not invocable with the provided arguments");
    if (!IsEmpty()) [[unlikely]] {
      throw std::runtime_error("Empty ActorRef, cannot call method on it.");
    }
    EXA_THROW_CHECK_EQ(node_id_, this_node_id_) << "Cannot call remote actor using SendLocal, use Send instead.";
    return type_erased_actor_->template CallActorMethod<kMethod>(std::forward<Args>(args)...);
  }

  bool IsEmpty() const { return is_empty_; }

  uint32_t GetNodeId() const { return node_id_; }
  uint64_t GetActorId() const { return actor_id_; }
  std::optional<uint32_t> ActorIdInNetworkProtocol() const { return class_index_in_roster_; }

 private:
  bool is_empty_;
  uint32_t this_node_id_ = 0;
  uint32_t node_id_ = 0;
  uint64_t actor_id_ = 0;
  uint64_t class_index_in_roster_ = 0;
  TypeErasedActor* type_erased_actor_ = nullptr;
  network::MessageBroker* message_broker_ = nullptr;
};

class LocalRunTimeInfo {
 public:
  static LocalRunTimeInfo& GetThreadLocalInstance() {
    static thread_local LocalRunTimeInfo instance {0, nullptr, nullptr};
    return instance;
  }

  static void InitThreadLocalInstacne(uint32_t this_node_id, std::function<TypeErasedActor*(uint64_t)> look_up,
                                      network::MessageBroker* broker) {
    auto& inst = GetThreadLocalInstance();
    if (inst.initialized_) {
      return;
    }
    inst.this_node_id_ = this_node_id;
    inst.look_up_ = std::move(look_up);
    inst.message_broker_ = broker;
    inst.initialized_ = true;
  }

  bool initialized_ = false;
  uint32_t this_node_id_;
  std::function<TypeErasedActor*(uint64_t)> look_up_;
  network::MessageBroker* message_broker_;

 private:
  LocalRunTimeInfo(uint32_t this_node_id, std::function<TypeErasedActor*(uint64_t)> look_up,
                   network::MessageBroker* message_broker)
      : this_node_id_(this_node_id), look_up_(std::move(look_up)), message_broker_(message_broker) {};
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
    uint64_t class_index_in_roster {};
  };

  static ex_actor::internal::ActorRef<U> to(const ReflType& rfl_type) noexcept {
    auto& info = ex_actor::internal::LocalRunTimeInfo::GetThreadLocalInstance();

    ex_actor::internal::ActorRef<U> actor(info.this_node_id_, rfl_type.node_id, rfl_type.actor_id,
                                          rfl_type.class_index_in_roster, info.look_up_(rfl_type.actor_id),
                                          info.message_broker_);
    return actor;
  }

  static ReflType from(const ex_actor::internal::ActorRef<U>& actor_ref) {
    return {.is_valid = actor_ref.is_empty_,
            .node_id = actor_ref.node_id_,
            .actor_id = actor_ref.actor_id_,
            .class_index_in_roster = actor_ref.class_index_in_roster_};
  }
};
}  // namespace rfl

namespace std {
template <class UserClass>
struct hash<ex_actor::ActorRef<UserClass>> {
  size_t operator()(const ex_actor::ActorRef<UserClass>& ref) const {
    if (!ref.IsEmpty()) {
      return ex_actor::internal::kEmptyActorRefHashVal;
    }
    return std::hash<uint64_t>()(ref.GetActorId()) ^ std::hash<uint32_t>()(ref.GetNodeId());
  }
};
}  // namespace std
