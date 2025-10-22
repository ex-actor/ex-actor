#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <queue>

#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

#include "ex_actor/internal/alias.h"  // IWYU pragma: keep
#include "ex_actor/internal/logging.h"

namespace ex_actor::util {
/**
 * @brief A std::execution style semaphore. support on_negative sender.
 */
class Semaphore {
 public:
  explicit Semaphore(int64_t initial_count) : count_(initial_count) {}

  int64_t Acquire(int64_t n) {
    auto prev = count_.fetch_sub(n, std::memory_order_release);
    if (prev <= n) {
      std::unique_lock lock(waiters_mu_);
      auto waiters = std::move(waiters_);
      lock.unlock();
      for (auto* waiter : waiters) {
        waiter->Complete();
      }
    }
    return prev - n;
  }

  int64_t Release(int64_t n) {
    auto prev = count_.fetch_add(n, std::memory_order_release);
    return prev + n;
  }

  int64_t CurrentValue() const { return count_.load(std::memory_order_acquire); }

  struct OnDrainedSender;

  /**
   * @brief Return a sender that will be completed when the semaphore is drained(value <= 0).
   */
  OnDrainedSender OnDrained() { return OnDrainedSender {.semaphore = this}; }

  struct TypeErasedOnDrainedOperation {
    virtual ~TypeErasedOnDrainedOperation() = default;
    virtual void Complete() = 0;
  };

  template <ex::receiver R>
  struct OnDrainedOperation : public TypeErasedOnDrainedOperation {
    OnDrainedOperation(Semaphore* semaphore, R receiver) : semaphore(semaphore), receiver(std::move(receiver)) {}
    Semaphore* semaphore;
    R receiver;

    void start() noexcept {
      // must get lock before checking count_, or if the count_ is drained before the waiter is queued but after
      // counter_ is checked, the waiter will never be completed
      std::scoped_lock lock(semaphore->waiters_mu_);
      if (semaphore->count_.load(std::memory_order_acquire) <= 0) {
        Complete();
      } else {
        semaphore->waiters_.push_back(this);
      }
    }

    void Complete() override { receiver.set_value(); }
  };

  struct OnDrainedSender : ex::sender_t {
    using completion_signatures = ex::completion_signatures<ex::set_value_t()>;
    Semaphore* semaphore;

    template <ex::receiver R>
    OnDrainedOperation<R> connect(R receiver) {
      return {semaphore, std::move(receiver)};
    }
  };

 private:
  mutable std::mutex waiters_mu_;
  std::vector<TypeErasedOnDrainedOperation*> waiters_;
  std::atomic_int64_t count_;
};

};  // namespace ex_actor::util

namespace ex_actor::internal::util {
template <class T>
struct LinearizableUnboundedQueue {
 public:
  void Push(T value) {
    std::lock_guard lock(mutex_);
    queue_.push(std::move(value));
  }

  std::optional<T> TryPop() {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) {
      return std::nullopt;
    }
    auto value = std::move(queue_.front());
    queue_.pop();
    return value;
  }

 private:
  std::queue<T> queue_;
  mutable std::mutex mutex_;
};

template <class K, class V>
class LockGuardedMap {
 public:
  bool Insert(const K& key, V value) {
    std::lock_guard lock(mutex_);
    auto [iter, inserted] = map_.try_emplace(key, std::move(value));
    return inserted;
  }

  V& At(const K& key) {
    std::lock_guard lock(mutex_);
    auto iter = map_.find(key);
    EXA_THROW_CHECK(iter != map_.end()) << "Key not found: " << key;
    return iter->second;
  }

  void Erase(const K& key) {
    std::lock_guard lock(mutex_);
    map_.erase(key);
  }

  std::mutex& GetMutex() const { return mutex_; }

 private:
  std::unordered_map<K, V> map_;
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

#if defined(_WIN32)
#include <windows.h>
inline void SetThreadName(const std::string& name) {
  SetThreadDescription(GetCurrentThread(), std::wstring(name.begin(), name.end()).c_str());
}
#elif defined(__linux__)
#include <pthread.h>
inline void SetThreadName(const std::string& name) { pthread_setname_np(pthread_self(), name.c_str()); }
#else
inline void SetThreadName(const std::string&) {}
#endif
}  // namespace ex_actor::internal::util