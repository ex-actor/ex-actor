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

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

#include "ex_actor/3rd_lib/daking/MPSC_queue.h"
#include "ex_actor/3rd_lib/moody_camel_queue/blockingconcurrentqueue.h"
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
struct LinearizableUnboundedMpscQueue {
 public:
  void Push(T value) { queue_.enqueue(std::move(value)); }

  std::optional<T> TryPop() {
    T value;
    if (queue_.try_dequeue(value)) {
      return value;
    }
    return std::nullopt;
  }

 private:
  ex_actor::embedded_3rd::daking::MPSC_queue<T> queue_;
};

template <class T>
class UnboundedBlockingPriorityQueue {
 public:
  void Push(T value, uint32_t priority) {
    std::lock_guard lock(mutex_);
    queue_.push({std::move(value), priority});
    cv_.notify_one();
  }

  std::optional<T> Pop(uint64_t timeout_ms) {
    std::unique_lock lock(mutex_);
    bool ok = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !queue_.empty(); });
    if (!ok) {
      return std::nullopt;
    }
    auto value = std::move(const_cast<Element&>(queue_.top()).value);
    queue_.pop();
    return value;
  }

 private:
  struct Element {
    T value;
    uint32_t priority;
    friend bool operator<(const Element& lhs, const Element& rhs) { return lhs.priority > rhs.priority; }
  };
  std::priority_queue<Element> queue_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

template <class T>
class UnboundedBlockingQueue {
 public:
  void Push(T value) { queue_.enqueue(std::move(value)); }
  std::optional<T> Pop(uint64_t timeout_ms) {
    T value;
    bool ok = queue_.wait_dequeue_timed(value, std::chrono::milliseconds(timeout_ms));
    if (!ok) {
      return std::nullopt;
    }
    return value;
  }

 private:
  ex_actor::embedded_3rd::moodycamel::BlockingConcurrentQueue<T> queue_;
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

  const V& At(const K& key) const {
    std::lock_guard lock(mutex_);
    auto iter = map_.find(key);
    EXA_THROW_CHECK(iter != map_.end()) << "Key not found: " << key;
    return iter->second;
  }

  void Erase(const K& key) {
    std::lock_guard lock(mutex_);
    map_.erase(key);
  }

  bool Contains(const K& key) const {
    std::lock_guard lock(mutex_);
    return map_.contains(key);
  }

  void Clear() {
    std::lock_guard lock(mutex_);
    map_.clear();
  }

  std::unordered_map<K, V>& GetMap() { return map_; }
  std::mutex& GetMutex() const { return mutex_; }

 private:
  std::unordered_map<K, V> map_;
  mutable std::mutex mutex_;
};

template <class T>
class LockGuardedSet {
 public:
  bool Insert(T value) {
    std::lock_guard lock(mutex_);
    auto [iter, inserted] = set_.emplace(std::move(value));
    return inserted;
  }

  void Erase(const T& value) {
    std::lock_guard lock(mutex_);
    set_.erase(value);
  }

  bool Empty() {
    std::lock_guard lock(mutex_);
    return set_.empty();
  }

  bool Contains(const T& value) {
    std::lock_guard lock(mutex_);
    return set_.contains(value);
  }

  std::mutex& GetMutex() const { return mutex_; }

 private:
  std::unordered_set<T> set_;
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
inline void SetThreadName(const std::string& name) {
  // TODO
}
#elif defined(__linux__)
#include <pthread.h>
inline void SetThreadName(const std::string& name) { pthread_setname_np(pthread_self(), name.c_str()); }
#else
inline void SetThreadName(const std::string&) {}
#endif

inline auto WrapSenderWithInlineScheduler(auto task) {
  return std::move(task) | stdexec::write_env(ex::prop {stdexec::get_scheduler, stdexec::inline_scheduler {}});
}
}  // namespace ex_actor::internal::util
