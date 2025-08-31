#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>

#include <stdexec/execution.hpp>

#include "ex_actor/detail/alias.h"  // IWYU pragma: keep

namespace ex_actor::detail {
template <class T>
struct ThreadSafeQueue {
 public:
  void Push(T value) {
    std::lock_guard lock(mutex_);
    queue_.push(std::move(value));
  }

  T Pop() {
    std::lock_guard lock(mutex_);
    auto value = std::move(queue_.front());
    queue_.pop();
    return value;
  }

  size_t Size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
  }

  bool Empty() const { return Size() == 0; }

 private:
  std::queue<T> queue_;
  mutable std::mutex mutex_;
};

// Similar to std::any, but allow move-only types
class MoveOnlyAny {
 public:
  MoveOnlyAny(const MoveOnlyAny&) = delete;
  MoveOnlyAny& operator=(const MoveOnlyAny&) = delete;
  MoveOnlyAny(MoveOnlyAny&&) = default;
  MoveOnlyAny& operator=(MoveOnlyAny&&) = default;

  MoveOnlyAny() : value_holder_(nullptr) {}
  template <class T>
  explicit MoveOnlyAny(T value) : value_holder_(std::make_unique<AnyValueHolderImpl<T>>(std::move(value))) {}
  template <class T>
  MoveOnlyAny& operator=(T value) {
    value_holder_ = std::make_unique<AnyValueHolderImpl<T>>(std::move(value));
    return *this;
  }

  template <class T>
  T&& MoveValueOut() && {
    return std::move(static_cast<AnyValueHolderImpl<T>*>(value_holder_.get())->value);
  }

 private:
  struct AnyValueHolder {
    virtual ~AnyValueHolder() = default;
  };

  template <class T>
  struct AnyValueHolderImpl : public AnyValueHolder {
    T value;
    explicit AnyValueHolderImpl(T value) : value(std::move(value)) {}
  };
  std::unique_ptr<AnyValueHolder> value_holder_;
};

}  // namespace ex_actor::detail