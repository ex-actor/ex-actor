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

#include <concepts>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <rfl/capnproto.hpp>
#include <spdlog/spdlog.h>

#include "ex_actor/internal/actor_config.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/reflect.h"

// ===================================================
// Add external context support to rfl::capnproto
// ===================================================

namespace rfl {
namespace capnproto {
template <class Ctx>
class ReaderWithContext : public Reader {
 public:
  const Ctx& info;
  explicit ReaderWithContext(const Ctx& info) : info(info) {}

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

template <class T, class Ctx, class... Ps>
auto read(const InputVarType& _obj, const Ctx& ctx) {
  const ReaderWithContext r {ctx};
  return rfl::parsing::Parser<ReaderWithContext<Ctx>, Writer, T, Processors<SnakeCaseToCamelCase, Ps...>>::read(r,
                                                                                                                _obj);
}

template <class T, class Ctx, class... Ps>
Result<internal::wrap_in_rfl_array_t<T>> read(const concepts::ByteLike auto* bytes, size_t size,
                                              const Schema<T>& schema, const Ctx& ctx) {
  const auto array_ptr = kj::ArrayPtr<const kj::byte>(internal::ptr_cast<const kj::byte*>(bytes), size);
  auto input_stream = kj::ArrayInputStream(array_ptr);
  auto message_reader = capnp::PackedMessageReader(input_stream);
  const auto root_name = get_root_name<std::remove_cv_t<T>, Ps...>();
  const auto root_schema = schema.value().getNested(root_name.c_str());
  const auto input_var = InputVarType {message_reader.getRoot<capnp::DynamicStruct>(root_schema.asStruct())};
  return read<T, Ctx, Ps...>(input_var, ctx);
}
}  // namespace capnproto
};  // namespace rfl

// ===================================================
// ex_actor's own serialization related code
// ===================================================
namespace ex_actor::internal {

class TypeErasedActor;
class MessageBroker;

struct ActorRefSerdeContext {
  uint32_t this_node_id = 0;
  std::function<TypeErasedActor*(uint64_t)> actor_look_up_fn;
  MessageBroker* message_broker = nullptr;
};

template <class T>
static auto GetCachedSchema() {
  thread_local auto schema = rfl::capnproto::to_schema<T>();
  return schema;
}

template <class T>
std::vector<char> Serialize(const T& obj) {
  return rfl::capnproto::write(obj, GetCachedSchema<T>());
}

template <class T>
T Deserialize(const uint8_t* data, size_t size) {
  return rfl::capnproto::read<T>(data, size, GetCachedSchema<T>()).value();
}

template <class T>
T Deserialize(const uint8_t* data, size_t size, const ActorRefSerdeContext& ctx) {
  return rfl::capnproto::read<T, ActorRefSerdeContext>(data, size, GetCachedSchema<T>(), ctx).value();
}

enum class NetworkRequestType : uint8_t {
  kActorCreationRequest = 0,
  kActorMethodCallRequest,
  kActorLookUpRequest,
};

enum class NetworkReplyType : uint8_t {
  kActorCreationReturn = 0,
  kActorCreationError,
  kActorMethodCallReturn,
  kActorMethodCallError,
  kActorLookUpReturn,
  kActorLookUpError,

};

template <class Tuple>
struct ActorCreationArgs {
  ActorConfig actor_config;
  Tuple args_tuple;
};

struct ActorCreationError {
  std::string error;
};

template <class Tuple>
struct ActorMethodCallArgs {
  Tuple args_tuple;
};

template <class T>
struct ActorMethodReturnValue {
  T return_value;
};

struct ActorMethodReturnError {
  std::string error;
};

template <>
struct ActorMethodReturnValue<void> {};

struct ActorLookUpRequest {
  std::string actor_name;
};

template <auto kFn>
auto DeserializeFnArgs(const uint8_t* data, size_t size, const ActorRefSerdeContext& info) {
  using Sig = Signature<decltype(kFn)>;
  if constexpr (std::is_member_function_pointer_v<decltype(kFn)>) {
    return Deserialize<ActorMethodCallArgs<typename Sig::DecayedArgsTupleType>>(data, size, info);
  } else {
    return Deserialize<ActorCreationArgs<typename Sig::DecayedArgsTupleType>>(data, size, info);
  }
}

struct MemoryBuf : std::streambuf {
  MemoryBuf(char const* base, size_t size) {
    char* p(const_cast<char*>(base));
    this->setg(p, p, p + size);
  }
};
struct InputMemoryStream : virtual MemoryBuf, std::istream {
  InputMemoryStream(char const* base, size_t size)
      : MemoryBuf(base, size), std::istream(static_cast<std::streambuf*>(this)) {}
};

template <class B>
class BufferReader {
 public:
  explicit BufferReader(B buffer)
      : buffer_(std::move(buffer)), start_(static_cast<uint8_t*>(buffer_.data())), size_(buffer_.size()) {}

  template <class T>
    requires std::integral<T> || std::floating_point<T> || std::is_enum_v<T>
  T NextPrimitive() {
    EXA_THROW_CHECK_LE(offset_ + sizeof(T), size_) << "Buffer overflow, offset: " << offset_ << ", size: " << size_;
    T value = *reinterpret_cast<const T*>(Current());
    offset_ += sizeof(T);
    return value;
  }

  std::string PullString(size_t length) {
    EXA_THROW_CHECK_LE(offset_ + length, size_) << "Buffer overflow, offset: " << offset_ << ", size: " << size_;
    std::string value(reinterpret_cast<const char*>(Current()), length);
    offset_ += length;
    return value;
  }

  const uint8_t* Current() const { return start_ + offset_; }

  size_t RemainingSize() const { return size_ - offset_; }

  InputMemoryStream ToInputMemoryStream() const {
    return InputMemoryStream(reinterpret_cast<const char*>(Current()), RemainingSize());
  }

 private:
  B buffer_;
  const uint8_t* start_;
  size_t size_;
  size_t offset_ = 0;
};

template <class B>
class BufferWriter {
 public:
  explicit BufferWriter(B buffer)
      : buffer_(std::move(buffer)), start_(static_cast<uint8_t*>(buffer_.data())), size_(buffer_.size()) {}

  template <class T>
    requires std::integral<T> || std::floating_point<T> || std::is_enum_v<T>
  void WritePrimitive(T value) {
    EXA_THROW_CHECK_LE(offset_ + sizeof(T), size_) << "Buffer overflow, offset: " << offset_ << ", size: " << size_;
    *reinterpret_cast<T*>(Current()) = value;
    offset_ += sizeof(T);
  }

  void CopyFrom(const uint8_t* data, size_t size) {
    EXA_THROW_CHECK_LE(offset_ + size, size_)
        << "Buffer overflow, offset: " << offset_ << ", size: " << size_ << ", size_to_copy: " << size;
    std::memcpy(Current(), data, size);
    offset_ += size;
  }
  void CopyFrom(const char* data, size_t size) { CopyFrom(reinterpret_cast<const uint8_t*>(data), size); }

  const B& GetBuffer() const { return buffer_; }

  B&& MoveBufferOut() && { return std::move(buffer_); }

  bool EndReached() const { return offset_ == size_; }

 private:
  uint8_t* Current() { return start_ + offset_; }

  B buffer_;
  uint8_t* start_;
  size_t size_;
  size_t offset_ = 0;
};

inline std::string BufferToHex(const uint8_t* data, size_t size) {
  std::stringstream ss;
  for (size_t i = 0; i < size; i++) {
    ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << ' ';
  }
  return ss.str();
}
inline std::string BufferToHex(const char* data, size_t size) {
  return BufferToHex(reinterpret_cast<const uint8_t*>(data), size);
}
}  // namespace ex_actor::internal
