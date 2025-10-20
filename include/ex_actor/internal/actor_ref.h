#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "ex_actor/internal/actor.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/network.h"
#include "ex_actor/internal/reflect.h"
#include "ex_actor/internal/serialization.h"

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

  /**
   * @brief Send message to an actor. Returns a coroutine carrying the result. Dynamic memory allocation will happen due
   * to the use of coroutine. If you can confirm it's a local actor, consider using SendLocal() instead, which has
   * better performance.
   * @note The returned coroutine is not copyable. please use `co_await std::move(coroutine)`.
   */
  template <auto kMethod, class... Args>
  auto Send(Args&&... args) const
      -> exec::task<typename decltype(reflect::UnwrapRetrunSenderIfNested<kMethod>())::type> {
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
    // protocol: [request_type][class_index_in_roster][method_index][actor_id][ActorMethodCallArgs]
    using Sig = reflect::Signature<decltype(kMethod)>;
    serde::ActorMethodCallArgs<typename Sig::DecayedArgsRflTupleType> method_call_args {
        .args_tuple = typename Sig::DecayedArgsRflTupleType(std::forward<Args>(args)...)};
    auto serialized_args = serde::Serialize(method_call_args);
    std::optional<uint64_t> optional_method_index = reflect::GetActorMethodIndex<kMethod>();
    EXA_THROW_CHECK(optional_method_index.has_value())
        << "Can't find method index in actor methods. Please put this method to your actor class's method tuple.";
    uint64_t method_index = optional_method_index.value();
    serde::BufferWriter buffer_writer(network::ByteBufferType {sizeof(serde::NetworkRequestType) +
                                                               sizeof(class_index_in_roster_) + sizeof(method_index) +
                                                               sizeof(actor_id_) + serialized_args.size()});
    buffer_writer.WritePrimitive(serde::NetworkRequestType::kActorMethodCallRequest);
    buffer_writer.WritePrimitive(class_index_in_roster_);
    buffer_writer.WritePrimitive(method_index);
    buffer_writer.WritePrimitive(actor_id_);
    buffer_writer.CopyFrom(serialized_args.data(), serialized_args.size());
    using UnwrappedType = decltype(reflect::UnwrapRetrunSenderIfNested<kMethod>())::type;
    auto sender = message_broker_->SendRequest(node_id_, std::move(buffer_writer).MoveBufferOut()) |
                  ex::then([](network::ByteBufferType response_buffer) {
                    if constexpr (std::is_void_v<UnwrappedType>) {
                      return;
                    } else {
                      return serde::Deserialize<serde::ActorMethodReturnValue<UnwrappedType>>(
                                 response_buffer.data<uint8_t>(), response_buffer.size())
                          .return_value;
                    }
                  });
    co_return co_await std::move(sender);
  }

  /**
   * @brief Send message to a local actor. Has better performance than the generic Send(). No heap allocation.
   */
  template <auto kMethod, class... Args>
  ex::sender auto SendLocal(Args&&... args) const {
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
}  // namespace ex_actor::internal

namespace ex_actor {
using internal::ActorRef;
}  // namespace ex_actor

namespace std {
template <class UserClass>
struct hash<ex_actor::ActorRef<UserClass>> {
  size_t operator()(const ex_actor::ActorRef<UserClass>& ref) const {
    if (!ref.IsEmpty()) {
      return 10086;
    }
    return std::hash<uint64_t>()(ref.GetActorId()) ^ std::hash<uint32_t>()(ref.GetNodeId());
  }
};
}  // namespace std
