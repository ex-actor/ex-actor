#pragma once

#include <rfl/capnproto.hpp>

#include "ex_actor/internal/actor_ref_serialization/actor_ref_deserialization_info.h"

namespace rfl {
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

}  // namespace rfl
