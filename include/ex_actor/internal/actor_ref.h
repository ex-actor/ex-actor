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
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ex_actor/internal/local_actor_ref.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/message.h"
#include "ex_actor/internal/network.h"
#include "ex_actor/internal/reflect.h"
#include "ex_actor/internal/serialization.h"

namespace ex_actor::internal {

template <typename Sender>
class [[nodiscard]] SendBuilder : public ex::sender_t {
 public:
  // Use stdexec::env<> instead of deprecated empty_env
  using completion_signatures = stdexec::completion_signatures_of_t<Sender, stdexec::env<>>;

  explicit SendBuilder(Sender&& sender, std::string op_name = "")
      : sender_(std::move(sender)), op_name_(std::move(op_name)) {}

  auto AttachDebugInfo(std::string_view message, std::source_location loc = std::source_location::current()) && {
    log::LogAttachDebugInfo(message, loc);
    return std::move(*this);
  }

  template <ex::receiver R>
  auto connect(R&& receiver) && {
    return ex::connect(std::move(sender_), std::forward<R>(receiver));
  }

  auto get_env() const noexcept { return stdexec::get_env(sender_); }

 private:
  Sender sender_;
  std::string op_name_;
};

template <class UserClass>
class ActorRef : public LocalActorRef<UserClass> {
 public:
  ActorRef() : LocalActorRef<UserClass>() {}

  ActorRef(uint64_t this_node_id, uint64_t node_id, uint64_t actor_id, TypeErasedActor* actor,
           const LocalActorRef<MessageBroker>& broker_actor_ref)
      : LocalActorRef<UserClass>(actor_id, actor),
        this_node_id_(this_node_id),
        node_id_(node_id),
        broker_actor_ref_(broker_actor_ref) {}

  friend bool operator==(const ActorRef& lhs, const ActorRef& rhs) {
    if (lhs.is_empty_ && rhs.is_empty_) {
      return true;
    }
    return lhs.node_id_ == rhs.node_id_ && lhs.actor_id_ == rhs.actor_id_;
  }

  void SetLocalRuntimeInfo(uint64_t this_node_id, TypeErasedActor* actor,
                           const LocalActorRef<MessageBroker>& broker_actor_ref) {
    this_node_id_ = this_node_id;
    this->type_erased_actor_ = actor;
    broker_actor_ref_ = broker_actor_ref;
  }

  friend rfl::Reflector<ActorRef<UserClass>>;

  template <class U>
  friend class ActorRef;

  // Converting constructor from ActorRef<U> where U* is convertible to UserClass*
  template <class Other>
    requires std::is_convertible_v<Other*, UserClass*>
  // NOLINTNEXTLINE(google-explicit-constructor) - implicit conversion is intentional for polymorphism support
  ActorRef(const ActorRef<Other>& other)
      : LocalActorRef<UserClass>(other),
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
   * @brief Send message to an actor. Returns a coroutine carrying the result.
   * @note This method requires your args and return value can be serialized by reflect-cpp, if you met compile errors
   * like "Unsupported type", refer https://rfl.getml.com/concepts/custom_classes/ to add a serializer for it. Or if you
   * can confirm it's a local actor, use SendLocal() instead, which doesn't require your args to be serializable.
   * @note Dynamic memory allocation will happen due to the use of coroutine. If you can confirm it's a local actor,
   * consider using SendLocal() instead, which has better performance.
   * @note The returned coroutine is not copyable. please use `co_await std::move(coroutine)`.
   */
  template <auto kMethod, class... Args>
  [[nodiscard]] auto Send(Args... args) const
    requires(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>)
  {
    // Add a fallback inline_scheduler for it.
    return SendBuilder(WrapSenderWithInlineScheduler(SendInternal<kMethod>(std::move(args)...)),
                       GetUniqueNameForFunction<kMethod>());
  }

  /**
   * @brief Send message to a local actor. Has better performance than the generic Send(). No heap allocation.
   */
  template <auto kMethod, class... Args>
  [[nodiscard]] ex::sender auto SendLocal(Args... args) const
    requires(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>)
  {
    EXA_THROW_CHECK_EQ(node_id_, this_node_id_) << "Cannot call remote actor using SendLocal, use Send instead.";
    return LocalActorRef<UserClass>::template SendLocal<kMethod>(std::move(args)...);
  }

  uint64_t GetNodeId() const { return node_id_; }

 private:
  uint64_t this_node_id_ = 0;
  uint64_t node_id_ = 0;
  LocalActorRef<MessageBroker> broker_actor_ref_;

  template <auto kMethod, class... Args>
  [[nodiscard]] auto SendInternal(Args... args) const
      -> exec::task<typename decltype(UnwrapReturnSenderIfNested<kMethod>())::type>
    requires(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>)
  {
    if (this->IsEmpty()) [[unlikely]] {
      throw std::runtime_error("Empty ActorRef, cannot call method on it.");
    }
    if (node_id_ == this_node_id_) {
      co_return co_await SendLocal<kMethod>(std::move(args)...);
    }

    // remote call
    EXA_THROW_CHECK(!broker_actor_ref_.IsEmpty()) << "Broker actor not set";
    using Sig = Signature<decltype(kMethod)>;
    ActorMethodCallArgs<typename Sig::DecayedArgsTupleType> method_call_args {
        .args_tuple = typename Sig::DecayedArgsTupleType(std::move(args)...)};

    NetworkRequest request {ActorMethodCallRequest {
        .handler_key = GetUniqueNameForFunction<kMethod>(),
        .actor_id = this->actor_id_,
        .serialized_args = Serialize(method_call_args),
    }};

    using UnwrappedType = decltype(UnwrapReturnSenderIfNested<kMethod>())::type;
    ByteBuffer response_buf =
        co_await broker_actor_ref_.SendLocal<&MessageBroker::SendRequest>(node_id_, Serialize(request));
    auto reply = Deserialize<NetworkReply>(response_buf);

    auto& ret = std::get<ActorMethodCallReply>(reply.variant);
    if (!ret.success) {
      EXA_THROW << "Got error from remote actor on node " << node_id_ << ": " << ret.error;
    }
    if constexpr (std::is_void_v<UnwrappedType>) {
      co_return;
    } else {
      auto res = Deserialize<ActorMethodReturnValue<UnwrappedType>>(ret.serialized_result);
      co_return res.return_value;
    }
  }
};

template <class UserClass>
void NotifyOnSpawned(TypeErasedActor* actor, const ActorRef<UserClass>& self_ref) {
  if constexpr (requires(UserClass& user_class, ActorRef<UserClass> self_ref) { user_class.OnSpawned(self_ref); }) {
    static_cast<UserClass*>(actor->GetUserClassInstanceAddress())->OnSpawned(self_ref);
  }
}

template <class UserClass>
void NotifyOnSpawned(TypeErasedActor* actor, const LocalActorRef<UserClass>& self_ref) {
  if constexpr (requires(UserClass& user_class, LocalActorRef<UserClass> self_ref) {
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
    return std::hash<uint64_t>()(ref.GetActorId()) ^ std::hash<uint64_t>()(ref.GetNodeId());
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
  LocalActorRef<MessageBroker> broker_actor_ref;
};
}  // namespace ex_actor::internal

namespace rfl {
template <typename U>
struct Reflector<ex_actor::internal::ActorRef<U>> {
  struct ReflType {
    bool is_empty {};
    uint64_t node_id {};
    uint64_t actor_id {};
  };

  static ex_actor::internal::ActorRef<U> to(const ReflType& rfl_type) noexcept {
    if (rfl_type.is_empty) {
      return ex_actor::internal::ActorRef<U>();
    }
    // this_node_id(0) and TypeErasedActor*(nullptr) are placeholders;
    // filled later in the Parser::read below.
    ex_actor::internal::ActorRef<U> actor(/*this_node_id=*/0, rfl_type.node_id, rfl_type.actor_id, /*actor=*/nullptr,
                                          /*broker_actor_ref=*/ {});
    return actor;
  }

  static ReflType from(const ex_actor::internal::ActorRef<U>& actor_ref) {
    return {
        .is_empty = actor_ref.is_empty_,
        .node_id = actor_ref.node_id_,
        .actor_id = actor_ref.actor_id_,
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
    actor_ref.SetLocalRuntimeInfo(info.this_node_id, info.actor_look_up_fn(actor_ref.GetActorId()),
                                  info.broker_actor_ref);
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
