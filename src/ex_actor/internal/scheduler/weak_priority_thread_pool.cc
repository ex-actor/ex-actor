// Copyright 2026 The ex_actor Authors.
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

#include "ex_actor/internal/scheduler/weak_priority_thread_pool.h"

#include <algorithm>
#include <bit>
#include <random>
#include <stdexcept>

#include "ex_actor/internal/platform.h"

namespace ex_actor {

namespace {

thread_local std::minstd_rand tl_rng {std::random_device {}()};

}  // namespace

static constexpr size_t kInitialSlotCapacity = 512;

WeakPriorityThreadPool::WeakPriorityThreadPool(size_t thread_count, size_t num_sub_queues)
    : thread_count_(thread_count),
      num_sub_queues_(std::max<size_t>(2, num_sub_queues == 0 ? thread_count / 2 : num_sub_queues)),
      sub_queues_(num_sub_queues_) {
  for (auto& sq : sub_queues_) {
    for (auto& slot : sq.slots) {
      slot.reserve(kInitialSlotCapacity);
    }
  }
  for (size_t i = 0; i < thread_count_; ++i) {
    workers_.emplace_back([this](const std::stop_token& stop_token) { WorkerThreadLoop(stop_token); });
  }
}

void WeakPriorityThreadPool::EnqueueOperation(TypeErasedOperation* operation, uint32_t priority) {
  if (priority >= kMaxPriorityLevels) {
    throw std::invalid_argument("WeakPriorityThreadPool: priority must be less than 64");
  }
  if (owning_pool_ == this) {
    if (local_slot_ == nullptr) {
      local_slot_ = operation;
      local_slot_priority_ = priority;
      return;
    }
    if (priority < local_slot_priority_) {
      auto* evicted = local_slot_;
      uint32_t evicted_priority = local_slot_priority_;
      local_slot_ = operation;
      local_slot_priority_ = priority;
      operation = evicted;
      priority = evicted_priority;
    }
  }
  size_t idx = tl_rng() % num_sub_queues_;
  {
    auto& sq = sub_queues_[idx];
    std::lock_guard guard(sq.lock);
    sq.slots[priority].push_back(operation);
    sq.bitmap.fetch_or(uint64_t {1} << priority, std::memory_order_relaxed);
  }
  sema_.signal();
}

WeakPriorityThreadPool::TypeErasedOperation* WeakPriorityThreadPool::TryDequeueOperation() {
  size_t idx_a = tl_rng() % num_sub_queues_;
  size_t idx_b = tl_rng() % num_sub_queues_;
  if (idx_b == idx_a) [[unlikely]] {
    idx_b = (idx_b + 1) % num_sub_queues_;
  }

  uint64_t bm_a = sub_queues_[idx_a].bitmap.load(std::memory_order_relaxed);
  uint64_t bm_b = sub_queues_[idx_b].bitmap.load(std::memory_order_relaxed);
  if (bm_a == 0 && bm_b == 0) {
    return nullptr;
  }

  size_t first = idx_a;
  size_t second = idx_b;
  if (bm_a == 0 || (bm_b != 0 && std::countr_zero(bm_b) < std::countr_zero(bm_a))) {
    first = idx_b;
    second = idx_a;
  }

  auto try_pop = [](SubQueue& sq) -> TypeErasedOperation* {
    std::lock_guard guard(sq.lock);
    uint64_t bm = sq.bitmap.load(std::memory_order_relaxed);
    if (bm == 0) {
      return nullptr;
    }
    size_t pri = std::countr_zero(bm);
    auto& slot = sq.slots[pri];
    TypeErasedOperation* op = slot.back();
    slot.pop_back();
    if (slot.empty()) {
      sq.bitmap.fetch_and(~(uint64_t {1} << pri), std::memory_order_relaxed);
    }
    return op;
  };

  TypeErasedOperation* op = try_pop(sub_queues_[first]);
  if (op != nullptr) {
    return op;
  }
  return try_pop(sub_queues_[second]);
}

void WeakPriorityThreadPool::WorkerThreadLoop(const std::stop_token& stop_token) {
  internal::SetThreadName("weak_pri_worker");
  owning_pool_ = this;

  while (!stop_token.stop_requested()) {
    if (!sema_.wait(static_cast<int64_t>(10) * 1000)) {
      continue;
    }

    TypeErasedOperation* operation = nullptr;
    while (operation == nullptr) {
      operation = TryDequeueOperation();
    }

    operation->Execute();
    // Immediately execute any task that was placed in the local slot
    // during Execute() (successor task from the same DAG chain).
    while (local_slot_ != nullptr) {
      auto* op = local_slot_;
      local_slot_ = nullptr;
      op->Execute();
    }
  }
  owning_pool_ = nullptr;
}

}  // namespace ex_actor
