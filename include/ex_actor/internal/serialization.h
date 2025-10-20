#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <rfl/capnproto.hpp>
#include <spdlog/spdlog.h>

#include "ex_actor/internal/actor.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/reflect.h"

namespace ex_actor::internal::serde {

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

enum class NetworkRequestType : uint8_t {
  kActorCreationRequest = 0,
  kActorMethodCallRequest,
};

template <class Tuple>
struct ActorCreationArgs {
  ActorConfig actor_config;
  Tuple args_tuple;
};

template <class Tuple>
struct ActorMethodCallArgs {
  Tuple args_tuple;
};

template <class T>
struct ActorMethodReturnValue {
  T return_value;
};

template <auto kFn>
auto DeserializeFnArgs(const uint8_t* data, size_t size) {
  using Sig = reflect::Signature<decltype(kFn)>;
  if constexpr (std::is_member_function_pointer_v<decltype(kFn)>) {
    return Deserialize<ActorMethodCallArgs<typename Sig::DecayedArgsRflTupleType>>(data, size);
  } else {
    return Deserialize<ActorCreationArgs<typename Sig::DecayedArgsRflTupleType>>(data, size);
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
}  // namespace ex_actor::internal::serde