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
#include "rfl/capnproto/Reader.hpp"
#include "rfl/capnproto/Writer.hpp"
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

  void SetLocalRuntimeInfo(uint32_t this_node_id, TypeErasedActor* actor, network::MessageBroker* message_broker) {
    this_node_id_ = this_node_id;
    type_erased_actor_ = actor;
    message_broker_ = message_broker;
  }

  friend rfl::Reflector<ActorRef<UserClass>>;

  /**
   * @brief Send message to an actor. Returns a coroutine carrying the result. Dynamic memory allocation will happen
   * due to the use of coroutine. If you can confirm it's a local actor, consider using SendLocal() instead, which
   * has better performance.
   * @note If your method argument can't be automatically serialized by reflect-cpp, refer
   * https://rfl.getml.com/concepts/custom_classes/ to add serializer for it. Or if you can confirm it's a local
   * actor, you can use SendLocal() to bypass the serialization code path.
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

namespace capnproto {

class ActorRefReader : public Reader {
 public:
  const ex_actor::internal::ActorRefDeserializationContext& info;
  explicit ActorRefReader(const ex_actor::internal::ActorRefDeserializationContext& info) : info(info) {}

  template <class VariantType, class UnionReaderType>
  rfl::Result<VariantType> read_union(const InputUnionType& u) const noexcept {
    const auto opt_pair = identify_discriminant(u);
    if (!opt_pair) {
      return rfl::error("Could not get the discriminant.");
    }
    const auto& [field, disc] = *opt_pair;
    return UnionReaderType::read(*this, disc, InputVarType {u.val_.get(field)});
  }

  std::optional<std::pair<capnp::StructSchema::Field, size_t>> identify_discriminant(
      const InputUnionType& _union) const noexcept {
    size_t ix = 0;
    for (auto field : _union.val_.getSchema().getFields()) {
      if (_union.val_.has(field)) {
        return std::make_pair(field, ix);
      }
      ++ix;
    }
    return std::optional<std::pair<capnp::StructSchema::Field, size_t>>();
  }
};

template <class T, class... Ps>
auto read(const InputVarType& _obj, const ex_actor::internal::ActorRefDeserializationContext& info) {
  const ActorRefReader r {info};
  return rfl::parsing::Parser<ActorRefReader, Writer, T, Processors<SnakeCaseToCamelCase, Ps...>>::read(r, _obj);
}

template <class T, class... Ps>
Result<internal::wrap_in_rfl_array_t<T>> read(const concepts::ByteLike auto* bytes, size_t size,
                                              const Schema<T>& schema,
                                              const ex_actor::internal::ActorRefDeserializationContext& info) {
  const auto array_ptr = kj::ArrayPtr<const kj::byte>(internal::ptr_cast<const kj::byte*>(bytes), size);
  auto input_stream = kj::ArrayInputStream(array_ptr);
  auto message_reader = capnp::PackedMessageReader(input_stream);
  const auto root_name = get_root_name<std::remove_cv_t<T>, Ps...>();
  const auto root_schema = schema.value().getNested(root_name.c_str());
  const auto input_var = InputVarType {message_reader.getRoot<capnp::DynamicStruct>(root_schema.asStruct())};
  return read<T, Ps...>(input_var, info);
}

}  // namespace capnproto

namespace parsing {
template <class ProcessorsType, class U>
struct Parser<capnproto::ActorRefReader, capnproto::Writer, ex_actor::internal::ActorRef<U>, ProcessorsType> {
  using Reader = capnproto::ActorRefReader;
  using InputVarType = typename Reader::InputVarType;

  static Result<ex_actor::internal::ActorRef<U>> read(const Reader& reader, const InputVarType& var) noexcept {
    using Type = typename Reflector<ex_actor::internal::ActorRef<U>>::ReflType;
    auto actor_ref =
        Parser<capnproto::Reader, capnproto::Writer, ex_actor::internal::ActorRef<U>, ProcessorsType>::read(reader, var)
            .value();
    const auto& info = reader.info;
    actor_ref.SetLocalRuntimeInfo(info.this_node_id, info.actor_look_up_fn(actor_ref.GetActorId()),
                                  info.message_broker);
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

namespace ex_actor::internal::serde {
template <class T>
T Deserialize(const uint8_t* data, size_t size, const ActorRefDeserializationContext& info) {
  return rfl::capnproto::read<T>(data, size, GetCachedSchema<T>(), info).value();
}

template <auto kFn>
auto DeserializeFnArgs(const uint8_t* data, size_t size, const ActorRefDeserializationContext& info) {
  using Sig = reflect::Signature<decltype(kFn)>;
  if constexpr (std::is_member_function_pointer_v<decltype(kFn)>) {
    return Deserialize<ActorMethodCallArgs<typename Sig::DecayedArgsTupleType>>(data, size, info);
  } else {
    return Deserialize<ActorCreationArgs<typename Sig::DecayedArgsTupleType>>(data, size, info);
  }
}

}  // namespace ex_actor::internal::serde
