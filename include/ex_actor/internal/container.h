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

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ex_actor/3rd_lib/daking/MPSC_queue.h"
#include "ex_actor/3rd_lib/moody_camel_queue/blockingconcurrentqueue.h"
#include "ex_actor/internal/logging.h"

namespace ex_actor::internal {

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

// Multiple single-threaded bounded queues sharing one contiguous buffer.
// All queue slots are laid out sequentially in a single allocation:
//   [queue0_slot0 .. queue0_slotN, queue1_slot0 .. queue1_slotN, ...]
// This avoids per-queue heap allocations and improves cache locality,
// especially when queue_size is small (e.g. 1).
// No synchronization — the caller must ensure exclusive access.
template <class T>
class FlatBoundedUnsafeQueues {
 public:
  FlatBoundedUnsafeQueues() = default;
  FlatBoundedUnsafeQueues(size_t queue_count, size_t queue_size)
      : queue_count_(queue_count), queue_size_(queue_size), buffer_(queue_count * queue_size), states_(queue_count) {}

  bool Push(size_t queue_index, T value) {
    auto& [head, tail, size] = states_[queue_index];
    if (size == queue_size_) {
      return false;
    }
    size_t base = queue_index * queue_size_;
    buffer_[base + tail] = std::move(value);
    tail = (tail + 1) % queue_size_;
    ++size;
    return true;
  }

  std::optional<T> TryPop(size_t queue_index) {
    auto& [head, tail, size] = states_[queue_index];
    if (size == 0) {
      return std::nullopt;
    }
    size_t base = queue_index * queue_size_;
    T value = std::move(buffer_[base + head]);
    head = (head + 1) % queue_size_;
    --size;
    return value;
  }

  size_t Size(size_t queue_index) const { return states_[queue_index].size; }
  bool Empty(size_t queue_index) const { return states_[queue_index].size == 0; }
  bool Full(size_t queue_index) const { return states_[queue_index].size == queue_size_; }
  size_t QueueCount() const { return queue_count_; }
  size_t QueueSize() const { return queue_size_; }

 private:
  struct QueueState {
    size_t head = 0;
    size_t tail = 0;
    size_t size = 0;
  };
  size_t queue_count_ {};
  size_t queue_size_ {};
  std::vector<T> buffer_;
  std::vector<QueueState> states_;
};
}  // namespace ex_actor::internal
