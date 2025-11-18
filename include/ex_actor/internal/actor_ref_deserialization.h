#pragma once

#include "actor_ref.h"
#include "network.h"

namespace ex_actor::internal {
struct ActorRefDeserializationInfo {
  uint32_t this_node_id = 0;
  std::function<TypeErasedActor*(uint64_t)> actor_look_up_fn;
  network::MessageBroker* message_broker = nullptr;
};
}  // namespace ex_actor::internal
//
namespace rfl {
template <typename U>
struct Reflector<ex_actor::internal::ActorRef<U>> {
  struct ReflType {
    bool is_valid {};
    uint32_t node_id {};
    uint64_t actor_id {};
  };

  static ex_actor::internal::ActorRef<U> to(const ReflType& rfl_type) noexcept {
    ex_actor::internal::ActorRef<U> actor(0, rfl_type.node_id, rfl_type.actor_id, nullptr, nullptr);

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
  const ex_actor::internal::ActorRefDeserializationInfo& info;
  explicit ActorRefReader(const ex_actor::internal::ActorRefDeserializationInfo& info) : info(info) {}

  template <class VariantType, class UnionReaderType>
  Result<VariantType> read_union(const InputUnionType& u) const noexcept {
    const auto opt_pair = identify_discriminant(u);
    if (!opt_pair) {
      return error("Could not get the discriminant.");
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
auto read(const InputVarType& _obj, const ex_actor::internal::ActorRefDeserializationInfo& info) {
  const ActorRefReader r {info};
  return rfl::parsing::Parser<ActorRefReader, Writer, T, Processors<SnakeCaseToCamelCase, Ps...>>::read(r, _obj);
}

template <class T, class... Ps>
Result<internal::wrap_in_rfl_array_t<T>> read(const concepts::ByteLike auto* bytes, size_t size,
                                              const Schema<T>& schema,
                                              const ex_actor::internal::ActorRefDeserializationInfo& info) {
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

namespace ex_actor::internal::serde {
template <class T>
T Deserialize(const uint8_t* data, size_t size, const ActorRefDeserializationInfo& info) {
  return rfl::capnproto::read<T>(data, size, GetCachedSchema<T>(), info).value();
}

template <auto kFn>
auto DeserializeFnArgs(const uint8_t* data, size_t size, const ActorRefDeserializationInfo& info) {
  using Sig = reflect::Signature<decltype(kFn)>;
  if constexpr (std::is_member_function_pointer_v<decltype(kFn)>) {
    return Deserialize<ActorMethodCallArgs<typename Sig::DecayedArgsTupleType>>(data, size, info);
  } else {
    return Deserialize<ActorCreationArgs<typename Sig::DecayedArgsTupleType>>(data, size, info);
  }
}

}  // namespace ex_actor::internal::serde
