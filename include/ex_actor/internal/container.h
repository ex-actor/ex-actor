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
#include <bit>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <queue>
#include <type_traits>
#include <utility>

#include "ex_actor/3rd_lib/daking/MPSC_queue.h"
#include "ex_actor/3rd_lib/moody_camel_queue/blockingconcurrentqueue.h"
#include "ex_actor/internal/alias.h"  // IWYU pragma: keep
#include "ex_actor/internal/logging.h"

namespace ex_actor::internal {

// Heap-allocated buffer with fixed capacity set at construction time. Storage is
// allocated up front but elements are NOT default-constructed; callers populate
// slots one at a time via EmplaceBack.
//
// Works with non-movable / non-copyable element types, unlike std::vector whose
// emplace_back requires the element type to be MoveInsertable (even when no
// reallocation happens).
template <class T>
class FixedCapacityBuffer {
 public:
  explicit FixedCapacityBuffer(size_t capacity)
      : capacity_(capacity),
        storage_(capacity == 0 ? nullptr
                               : static_cast<T*>(::operator new(sizeof(T) * capacity, std::align_val_t {alignof(T)}))) {
  }

  ~FixedCapacityBuffer() {
    for (size_t i = 0; i < size_; ++i) {
      storage_[i].~T();
    }
    ::operator delete(storage_, std::align_val_t {alignof(T)});
  }

  FixedCapacityBuffer(const FixedCapacityBuffer&) = delete;
  FixedCapacityBuffer& operator=(const FixedCapacityBuffer&) = delete;

  FixedCapacityBuffer(FixedCapacityBuffer&& other) noexcept
      : capacity_(other.capacity_), size_(other.size_), storage_(other.storage_) {
    other.capacity_ = 0;
    other.size_ = 0;
    other.storage_ = nullptr;
  }
  FixedCapacityBuffer& operator=(FixedCapacityBuffer&& other) noexcept {
    if (this != &other) {
      for (size_t i = 0; i < size_; ++i) {
        storage_[i].~T();
      }
      ::operator delete(storage_, std::align_val_t {alignof(T)});
      capacity_ = other.capacity_;
      size_ = other.size_;
      storage_ = other.storage_;
      other.capacity_ = 0;
      other.size_ = 0;
      other.storage_ = nullptr;
    }
    return *this;
  }

  template <class... Args>
  T& EmplaceBack(Args&&... args) {
    EXA_THROW_CHECK(size_ < capacity_) << "FixedCapacityBuffer is full, capacity=" << capacity_;
    T* slot = ::new (static_cast<void*>(storage_ + size_)) T(std::forward<Args>(args)...);
    ++size_;
    return *slot;
  }

  T& operator[](size_t index) { return storage_[index]; }
  const T& operator[](size_t index) const { return storage_[index]; }

  size_t Size() const { return size_; }
  size_t Capacity() const { return capacity_; }

 private:
  size_t capacity_;
  size_t size_ = 0;
  T* storage_;
};

template <class T>
struct LinearizableUnboundedMpscQueue {
 public:
  // Returns true unconditionally; signature mirrors bounded queues so callers can dispatch uniformly.
  bool Push(T value) {
    queue_.enqueue(std::move(value));
    return true;
  }

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

// Single-slot bounded queue. No synchronization — the caller must ensure exclusive access.
template <class T>
class OneSlotUnsafeQueue {
 public:
  // Returns false when the slot is already occupied.
  bool Push(T value) {
    if (has_value_) {
      return false;
    }
    slot_ = std::move(value);
    has_value_ = true;
    return true;
  }

  std::optional<T> TryPop() {
    if (!has_value_) {
      return std::nullopt;
    }
    has_value_ = false;
    return std::move(slot_);
  }

 private:
  T slot_ {};
  bool has_value_ = false;
};

// Multiple Producer Single Consumer bounded ring buffer.
// Push() is thread-safe for concurrent producers.
// Pop() / TryPop() must be called by exactly one consumer thread.
// This implementation is inspired by Dmitry Vyukov's MPMC bounded queue adapted here as a
// single-consumer variant with C++20 idioms.
// (https://github.com/craflin/LockFreeQueue/blob/master/mpmc_bounded_queue.h)
template <typename T>
  requires std::is_nothrow_move_assignable_v<T>
class BoundedMpscQueue {
 public:
// GCC warns that hardware_destructive_interference_size may vary with -mtune;
// for an internal-use type this is acceptable — silence the warning locally.
#if defined(__cpp_lib_hardware_interference_size)
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
  static constexpr std::size_t kCacheLineSize = std::hardware_destructive_interference_size;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#else
  static constexpr std::size_t kCacheLineSize = 64;
#endif

  explicit BoundedMpscQueue(std::size_t capacity)
      : capacity_(std::bit_ceil(std::max(capacity, std::size_t {2}))),
        mask_(capacity_ - 1),
        buffer_(std::make_unique<cell_t[]>(capacity_)) {
    for (std::size_t i = 0; i < capacity_; ++i) buffer_[i].sequence.store(i, std::memory_order::relaxed);
    enqueue_pos_.store(0, std::memory_order::relaxed);
    dequeue_pos_ = 0;
  }

  BoundedMpscQueue(const BoundedMpscQueue&) = delete;
  BoundedMpscQueue& operator=(const BoundedMpscQueue&) = delete;

  // Thread-safe: immutable after construction.
  [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }

  // Only safe to call from the consumer thread.
  // No underflow risk: the consumer owns dequeue_pos_ and enqueue_pos_ >= dequeue_pos_ is
  // invariant from the consumer's perspective.
  [[nodiscard]] std::size_t Size() const noexcept {
    return enqueue_pos_.load(std::memory_order::acquire) - dequeue_pos_;
  }

  // Returns false if the queue is full.
  template <typename U>
    requires std::convertible_to<U, T> && std::is_nothrow_assignable_v<T&, U&&>
  bool Push(U&& data) {
    std::size_t pos = enqueue_pos_.load(std::memory_order::relaxed);
    cell_t* cell;
    for (;;) {
      cell = &buffer_[pos & mask_];
      std::size_t seq = cell->sequence.load(std::memory_order::acquire);
      auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order::relaxed)) [[likely]] {
          break;
        }
      } else if (diff < 0) {
        return false;  // queue full
      } else {
        pos = enqueue_pos_.load(std::memory_order::relaxed);
      }
    }
    cell->data = std::forward<U>(data);
    cell->sequence.store(pos + 1, std::memory_order::release);
    return true;
  }

  // Convenience wrapper returning std::optional.
  [[nodiscard]] std::optional<T> TryPop() {
    T data;
    if (Pop(data)) {
      return std::optional<T> {std::move(data)};
    }
    return std::nullopt;
  }

 private:
  // Single consumer only. Returns false if the queue is empty.
  bool Pop(T& data) {
    cell_t* cell = &buffer_[dequeue_pos_ & mask_];
    std::size_t seq = cell->sequence.load(std::memory_order::acquire);
    auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(dequeue_pos_ + 1);
    if (diff < 0) [[unlikely]] {
      return false;  // queue empty
    }
    // diff > 0 should never happen with a single consumer.
    EXA_THROW_CHECK(diff == 0) << "BoundedMpscQueue: unexpected sequence state in Pop, diff=" << diff;
    data = std::move(cell->data);
    cell->sequence.store(dequeue_pos_ + capacity_, std::memory_order::release);
    ++dequeue_pos_;
    return true;
  }

  struct alignas(kCacheLineSize) cell_t {
    std::atomic<std::size_t> sequence;
    T data;
  };

  // Read-only after construction — shared freely across threads.
  std::size_t const capacity_;
  std::size_t const mask_;
  std::unique_ptr<cell_t[]> const buffer_;

  // Written by producers; read by producers and (rarely) consumer.
  alignas(kCacheLineSize) std::atomic<std::size_t> enqueue_pos_;

  // Written and read exclusively by the single consumer thread.
  alignas(kCacheLineSize) std::size_t dequeue_pos_;
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

// Lock-free bucketed priority queue. One ConcurrentQueue per priority level eliminates
// contention on Push entirely. Pop uses a LightweightSemaphore (spin-then-futex) for
// blocking — no mutex involved.
template <class T>
class BucketedPriorityQueue {
 public:
  explicit BucketedPriorityQueue(uint32_t max_priorities) : max_priorities_(max_priorities), buckets_(max_priorities) {}

  void Push(T value, uint32_t priority) {
    EXA_THROW_CHECK(priority < max_priorities_)
        << fmt_lib::format("priority={} is out of range, max={}", priority, max_priorities_);
    buckets_[priority].enqueue(std::move(value));
    sema_.signal();
  }

  std::optional<T> Pop(uint64_t timeout_ms) {
    if (!sema_.wait(static_cast<int64_t>(timeout_ms) * 1000)) {
      return std::nullopt;
    }
    T value;
    for (auto& bucket : buckets_) {
      if (bucket.try_dequeue(value)) {
        return value;
      }
    }
    return std::nullopt;
  }

 private:
  uint32_t max_priorities_;
  std::vector<ex_actor::embedded_3rd::moodycamel::ConcurrentQueue<T>> buckets_;
  ex_actor::embedded_3rd::moodycamel::LightweightSemaphore sema_;
};

}  // namespace ex_actor::internal
