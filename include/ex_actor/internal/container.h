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

#include "ex_actor/3rd_lib/daking/MPSC_queue.h"
#include "ex_actor/3rd_lib/moody_camel_queue/blockingconcurrentqueue.h"
#include "ex_actor/internal/logging.h"

namespace ex_actor::internal {

template <class T, size_t kN = 256>
class SmallVector {
 public:
  SmallVector() : capacity_(kN) {}

  explicit SmallVector(size_t count) : size_(count) {
    if (count > kN) {
      heap_data_ = new T[count];
      capacity_ = count;
      using_heap_ = true;
    } else {
      capacity_ = kN;
    }
  }

  SmallVector(size_t count, const T& value) : SmallVector(count) { std::fill(begin(), end(), value); }

  template <class Iter>
  SmallVector(Iter first, Iter last) : capacity_(kN) {
    for (auto it = first; it != last; ++it) {
      push_back(*it);
    }
  }

  SmallVector(const SmallVector& other)
      : size_(other.size_), capacity_(other.capacity_), using_heap_(other.using_heap_) {
    if (using_heap_) {
      heap_data_ = new T[capacity_];
    }
    std::copy(other.begin(), other.end(), begin());
  }

  SmallVector(SmallVector&& other) noexcept
      : size_(other.size_), capacity_(other.capacity_), using_heap_(other.using_heap_) {
    if (using_heap_) {
      heap_data_ = other.heap_data_;
      other.heap_data_ = nullptr;
      other.using_heap_ = false;
    } else {
      std::move(other.begin(), other.end(), begin());
    }
    other.size_ = 0;
    other.capacity_ = kN;
  }

  SmallVector& operator=(const SmallVector& other) {
    if (this == &other) {
      return *this;
    }
    SmallVector temp(other);
    swap(temp);
    return *this;
  }

  SmallVector& operator=(SmallVector&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    if (using_heap_) {
      delete[] heap_data_;
    }
    size_ = other.size_;
    capacity_ = other.capacity_;
    using_heap_ = other.using_heap_;
    if (using_heap_) {
      heap_data_ = other.heap_data_;
      other.heap_data_ = nullptr;
      other.using_heap_ = false;
    } else {
      std::move(other.begin(), other.end(), begin());
    }
    other.size_ = 0;
    other.capacity_ = kN;
    return *this;
  }

  ~SmallVector() {
    if (using_heap_) {
      delete[] heap_data_;
    }
  }

  void push_back(const T& value) {
    if (size_ >= capacity_) {
      grow(std::max(size_ + 1, capacity_ * 2));
    }
    data()[size_] = value;
    ++size_;
  }

  void push_back(T&& value) {
    if (size_ >= capacity_) {
      grow(std::max(size_ + 1, capacity_ * 2));
    }
    data()[size_] = std::move(value);
    ++size_;
  }

  void pop_back() {
    EXA_THROW_CHECK(!empty());
    --size_;
  }

  void resize(size_t new_size) {
    if (new_size > capacity_) {
      grow(new_size);
    }
    size_ = new_size;
  }

  void resize(size_t new_size, const T& value) {
    if (new_size > capacity_) {
      grow(new_size);
    }
    if (new_size > size_) {
      std::fill(data() + size_, data() + new_size, value);
    }
    size_ = new_size;
  }

  void clear() { size_ = 0; }

  T* data() { return using_heap_ ? heap_data_ : stack_data_; }
  const T* data() const { return using_heap_ ? heap_data_ : stack_data_; }

  T* begin() { return data(); }
  const T* begin() const { return data(); }

  T* end() { return data() + size_; }
  const T* end() const { return data() + size_; }

  T& operator[](size_t idx) {
    EXA_THROW_CHECK(idx < size_);
    return data()[idx];
  }
  const T& operator[](size_t idx) const {
    EXA_THROW_CHECK(idx < size_);
    return data()[idx];
  }

  T& front() {
    EXA_THROW_CHECK(!empty());
    return data()[0];
  }
  const T& front() const {
    EXA_THROW_CHECK(!empty());
    return data()[0];
  }

  T& back() {
    EXA_THROW_CHECK(!empty());
    return data()[size_ - 1];
  }
  const T& back() const {
    EXA_THROW_CHECK(!empty());
    return data()[size_ - 1];
  }

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  size_t capacity() const { return capacity_; }
  bool is_using_heap() const { return using_heap_; }

  void swap(SmallVector& other) noexcept {
    if (using_heap_ && other.using_heap_) {
      std::swap(heap_data_, other.heap_data_);
    } else if (!using_heap_ && !other.using_heap_) {
      std::swap_ranges(begin(), begin() + std::max(size_, other.size_), other.begin());
    } else if (using_heap_ && !other.using_heap_) {
      T* temp_heap = heap_data_;
      std::move(other.stack_data_, other.stack_data_ + other.size_, stack_data_);
      other.heap_data_ = temp_heap;
    } else {
      // !using_heap_ && other.using_heap_
      T* temp_heap = other.heap_data_;
      std::move(stack_data_, stack_data_ + size_, other.stack_data_);
      heap_data_ = temp_heap;
    }
    std::swap(size_, other.size_);
    std::swap(capacity_, other.capacity_);
    std::swap(using_heap_, other.using_heap_);
  }

 private:
  void grow(size_t new_capacity) {
    if (new_capacity <= capacity_) {
      return;
    }
    T* new_heap = new T[new_capacity];
    std::move(begin(), end(), new_heap);
    if (using_heap_) {
      delete[] heap_data_;
    }
    heap_data_ = new_heap;
    using_heap_ = true;
    capacity_ = new_capacity;
  }

 public:
  size_t size_ = 0;
  size_t capacity_ = kN;
  bool using_heap_ = false;

 private:
  union {
    T stack_data_[kN];
    T* heap_data_;
  };
};

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

}  // namespace ex_actor::internal
