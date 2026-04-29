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
#include <functional>
#include <type_traits>
#include <utility>
#include <variant>

#include "ex_actor/internal/basic_actor_ref.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/message.h"
#include "ex_actor/internal/network.h"
#include "ex_actor/internal/reflect.h"
#include "ex_actor/internal/serialization.h"
#include "ex_actor/internal/util.h"

namespace ex_actor::internal {
template <class UserClass>
class ActorRef : public BasicActorRef<UserClass> {
 public:
  ActorRef() : BasicActorRef<UserClass>() {}

  ActorRef(uint64_t this_node_id, uint64_t node_id, uint64_t actor_id, TypeErasedActor* actor,
           const BasicActorRef<MessageBroker>& broker_actor_ref)
      : BasicActorRef<UserClass>(actor_id, actor),
        this_node_id_(this_node_id),
        node_id_(node_id),
        broker_actor_ref_(broker_actor_ref) {}

  ActorRef(uint64_t this_node_id, uint64_t node_id, uint64_t actor_id, uint64_t actor_type_hash, TypeErasedActor* actor,
           const BasicActorRef<MessageBroker>& broker_actor_ref)
      : BasicActorRef<UserClass>(actor_id, actor, actor_type_hash),
        this_node_id_(this_node_id),
        node_id_(node_id),
        broker_actor_ref_(broker_actor_ref) {}

  friend bool operator==(const ActorRef& lhs, const ActorRef& rhs) {
    if (lhs.is_empty_ != rhs.is_empty_) {
      return false;
    }
    if (lhs.is_empty_) {
      return true;
    }
    return lhs.node_id_ == rhs.node_id_ && lhs.actor_id_ == rhs.actor_id_ &&
           lhs.actor_type_hash_ == rhs.actor_type_hash_;
  }

  void SetLocalRuntimeInfo(uint64_t this_node_id, TypeErasedActor* actor,
                           const BasicActorRef<MessageBroker>& broker_actor_ref) {
    this_node_id_ = this_node_id;
    this->type_erased_actor_ = actor;
    broker_actor_ref_ = broker_actor_ref;
  }

  void SetLocalRuntimeInfo(uint64_t this_node_id, uint64_t actor_type_hash, TypeErasedActor* actor,
                           const BasicActorRef<MessageBroker>& broker_actor_ref) {
    this_node_id_ = this_node_id;
    this->actor_type_hash_ = actor_type_hash;
    this->type_erased_actor_ = actor;
    broker_actor_ref_ = broker_actor_ref;
  }

  friend rfl::Reflector<ActorRef<UserClass>>;

  template <class U>
  friend class ActorRef;

  /**
   * @brief A builder targeting a specific mailbox queue. Obtained via Mailbox(index).
   * Extends BasicActorRef::SendBuilder with Send() for remote-capable refs.
   */
  class SendBuilder : public BasicActorRef<UserClass>::SendBuilder {
   public:
    SendBuilder(const ActorRef* ref, size_t mailbox_index)
        : BasicActorRef<UserClass>::SendBuilder(ref, mailbox_index), actor_ref_(ref) {}

    /**
     * @brief Send message to a specific mailbox queue. Returns a sender carrying the result.
     * @note Same serialization requirements as ActorRef::Send().
     */
    template <auto kMethod, class... Args>
    [[nodiscard]] ex::sender auto Send(Args... args) const
      requires(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>)
    {
      using LocalSenderType =
          decltype(std::declval<const SendBuilder*>()->template SendLocal<kMethod>(std::declval<Args>()...));
      using RemoteSenderType = decltype(actor_ref_->template SendRemote<kMethod>(std::declval<Args>()...));

      EXA_THROW_CHECK(!actor_ref_->IsEmpty()) << "Empty ActorRef, cannot call method on it.";

      using VariantType = std::variant<LocalSenderType, RemoteSenderType>;
      VariantType chosen_sender = (actor_ref_->node_id_ == actor_ref_->this_node_id_)
                                      ? VariantType(this->template SendLocal<kMethod>(std::move(args)...))
                                      : VariantType(actor_ref_->template SendRemote<kMethod>(std::move(args)...));

      return SenderVariant {std::move(chosen_sender)};
    }

   private:
    const ActorRef* actor_ref_;
  };

  /**
   * @brief Returns a SendBuilder targeting a specific mailbox queue of this actor.
   * Usage: actor_ref.Mailbox(index).Send<&Method>(args...)
   */
  SendBuilder Mailbox(size_t index) const { return SendBuilder(this, index); }

  // Converting constructor from ActorRef<U> where U* is convertible to UserClass*
  template <class Other>
    requires std::is_convertible_v<Other*, UserClass*>
  // NOLINTNEXTLINE(google-explicit-constructor) - implicit conversion is intentional for polymorphism support
  ActorRef(const ActorRef<Other>& other)
      : BasicActorRef<UserClass>(other),
        this_node_id_(other.this_node_id_),
        node_id_(other.node_id_),
        broker_actor_ref_(other.broker_actor_ref_) {}

  // Converting assignment operator - delegates to converting constructor
  template <class Other>
    requires std::is_convertible_v<Other*, UserClass*>
  ActorRef<UserClass>& operator=(const ActorRef<Other>& other) {
    *this = ActorRef<UserClass>(other);
    return *this;
  }

  /**
   * @brief Send message to an actor. Returns a sender carrying the result.
   * @note This method requires your args and return value can be serialized by reflect-cpp, if you met compile errors
   * like "Unsupported type", refer https://rfl.getml.com/concepts/custom_classes/ to add a serializer for it. Or if you
   * can confirm it's a local actor, use SendLocal() instead, which doesn't require your args to be serializable.
   */
  template <auto kMethod, class... Args>
  [[nodiscard]] ex::sender auto Send(Args... args) const
    requires(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>)
  {
    return Mailbox(0).template Send<kMethod>(std::move(args)...);
  }

  /**
   * @brief Send message to a local actor. Has better performance than the generic Send(). No heap allocation.
   */
  template <auto kMethod, class... Args>
  [[nodiscard]] ex::sender auto SendLocal(Args... args) const
    requires(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>)
  {
    EXA_THROW_CHECK_EQ(node_id_, this_node_id_) << "Cannot call remote actor using SendLocal, use Send instead.";
    return Mailbox(0).template SendLocal<kMethod>(std::move(args)...);
  }

  uint64_t GetNodeId() const { return node_id_; }

 private:
  template <auto kMethod, class... Args>
  [[nodiscard]] ex::sender auto SendRemote(Args... args) const {
    using UnwrappedType = typename decltype(UnwrapReturnSenderIfNested<kMethod>())::type;
    using Sig = Signature<decltype(kMethod)>;
    EXA_THROW_CHECK(!broker_actor_ref_.IsEmpty()) << "Broker actor not set";
    ActorMethodCallArgs<typename Sig::DecayedArgsTupleType> method_call_args {
        .args_tuple = typename Sig::DecayedArgsTupleType(std::move(args)...)};

    NetworkRequest request {ActorMethodCallRequest {
        .handler_key = ComputeRemoteMethodHandlerKey<kMethod>(this->actor_type_hash_),
        .actor_id = this->actor_id_,
        .serialized_args = Serialize(method_call_args),
    }};

    return broker_actor_ref_.template SendLocal<&MessageBroker::SendRequest>(node_id_, Serialize(request)) |
           ex::then([node_id = node_id_](ByteBuffer response_buf) -> UnwrappedType {
             auto reply = Deserialize<NetworkReply>(std::move(response_buf));
             auto& ret = std::get<ActorMethodCallReply>(reply.variant);
             if (!ret.success) {
               EXA_THROW << "Got error from remote actor on node " << node_id << ": " << ret.error;
             }
             if constexpr (std::is_void_v<UnwrappedType>) {
               return;
             } else {
               auto res = Deserialize<ActorMethodReturnValue<UnwrappedType>>(ret.serialized_result);
               return std::move(res.return_value);
             }
           });
  }

  uint64_t this_node_id_ = 0;
  uint64_t node_id_ = 0;
  BasicActorRef<MessageBroker> broker_actor_ref_;
};

template <class UserClass>
void NotifyOnSpawned(TypeErasedActor* actor, const ActorRef<UserClass>& self_ref) {
  if constexpr (requires(UserClass& user_class, ActorRef<UserClass> self_ref) { user_class.OnSpawned(self_ref); }) {
    static_cast<UserClass*>(actor->GetUserClassInstanceAddress())->OnSpawned(self_ref);
  }
}

template <class UserClass>
void NotifyOnSpawned(TypeErasedActor* actor, const BasicActorRef<UserClass>& self_ref) {
  if constexpr (requires(UserClass& user_class, BasicActorRef<UserClass> self_ref) {
                  user_class.OnSpawned(self_ref);
                }) {
    static_cast<UserClass*>(actor->GetUserClassInstanceAddress())->OnSpawned(self_ref);
  }
}
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
    uint64_t h = std::hash<uint64_t>()(ref.GetNodeId());
    h = ex_actor::internal::HashCombine(h, std::hash<uint64_t>()(ref.GetActorId()));
    h = ex_actor::internal::HashCombine(h, std::hash<uint64_t>()(ref.GetActorTypeHash()));
    return h;
  }
};
}  // namespace std

// ==============================
// rfl serialization support
// ==============================

namespace ex_actor::internal {
struct ActorRefSerdeContext {
  uint64_t this_node_id = 0;
  std::function<TypeErasedActor*(uint64_t)> actor_look_up_fn;
  BasicActorRef<MessageBroker> broker_actor_ref;
};
}  // namespace ex_actor::internal

namespace rfl {
template <typename U>
struct Reflector<ex_actor::internal::ActorRef<U>> {
  struct ReflType {
    bool is_empty {};
    uint64_t node_id {};
    uint64_t actor_id {};
    uint64_t actor_type_hash {};
    uint64_t adjusted_ptr_addr {};
  };

  static ex_actor::internal::ActorRef<U> to(const ReflType& rfl_type) noexcept {
    if (rfl_type.is_empty) {
      return ex_actor::internal::ActorRef<U>();
    }
    // this_node_id(0) and TypeErasedActor*(nullptr) are placeholders;
    // filled later in the Parser::read below.
    ex_actor::internal::ActorRef<U> actor(/*this_node_id=*/0, rfl_type.node_id, rfl_type.actor_id,
                                          rfl_type.actor_type_hash, /*actor=*/nullptr,
                                          /*broker_actor_ref=*/ {});
    // Restore the already-adjusted pointer from the sender side.
    // This is only valid for local actors; remote actors never use adjusted_ptr_.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    actor.adjusted_ptr_ = reinterpret_cast<U*>(rfl_type.adjusted_ptr_addr);
    return actor;
  }

  static ReflType from(const ex_actor::internal::ActorRef<U>& actor_ref) {
    return {
        .is_empty = actor_ref.is_empty_,
        .node_id = actor_ref.node_id_,
        .actor_id = actor_ref.actor_id_,
        .actor_type_hash = actor_ref.actor_type_hash_,
        .adjusted_ptr_addr = reinterpret_cast<uint64_t>(actor_ref.adjusted_ptr_),
    };
  }
};

namespace parsing {
template <class ProcessorsType, class U>
struct Parser<capnproto::ReaderWithContext<ex_actor::internal::ActorRefSerdeContext>, capnproto::Writer,
              ex_actor::internal::ActorRef<U>, ProcessorsType> {
  using Reader = capnproto::ReaderWithContext<ex_actor::internal::ActorRefSerdeContext>;
  using InputVarType = typename Reader::InputVarType;

  static Result<ex_actor::internal::ActorRef<U>> read(const Reader& reader, const InputVarType& var) noexcept {
    using Type = typename Reflector<ex_actor::internal::ActorRef<U>>::ReflType;
    auto parse_res =
        Parser<capnproto::Reader, capnproto::Writer, ex_actor::internal::ActorRef<U>, ProcessorsType>::read(reader,
                                                                                                            var);
    if (!parse_res) {
      return Unexpected {parse_res.error()};
    }
    auto actor_ref = parse_res.value();
    const auto& info = reader.info;
    actor_ref.SetLocalRuntimeInfo(info.this_node_id, actor_ref.GetActorTypeHash(),
                                  info.actor_look_up_fn(actor_ref.GetActorId()), info.broker_actor_ref);

    return actor_ref;
  }

  template <class Parent>
  static void write(const capnproto::Writer& w, const ex_actor::internal::ActorRef<U>& ref, const Parent& parent) {
    Parser<capnproto::Reader, capnproto::Writer, ex_actor::internal::ActorRef<U>, ProcessorsType>::write(w, ref,
                                                                                                         parent);
  }
};

}  // namespace parsing
}  // namespace rfl
