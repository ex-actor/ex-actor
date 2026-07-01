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
#include <random>

#include "ex_actor/internal/platform.h"

namespace ex_actor {

namespace {

thread_local std::minstd_rand tl_rng {std::random_device {}()};

}  // namespace

WeakPriorityThreadPool::WeakPriorityThreadPool(size_t thread_count, size_t num_sub_queues)
    : thread_count_(thread_count),
      num_sub_queues_(std::max<size_t>(2, num_sub_queues == 0 ? thread_count / 2 : num_sub_queues)),
      sub_queues_(num_sub_queues_) {
  for (size_t i = 0; i < thread_count_; ++i) {
    workers_.emplace_back([this](const std::stop_token& stop_token) { WorkerThreadLoop(stop_token); });
  }
}

void WeakPriorityThreadPool::EnqueueOperation(TypeErasedOperation* operation, uint32_t priority) {
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
  size_t idx_a = tl_rng() % num_sub_queues_;
  size_t idx_b = tl_rng() % num_sub_queues_;
  if (idx_a == idx_b) {
    idx_b = (idx_b + 1) % num_sub_queues_;
  }
  size_t idx =
      sub_queues_[idx_a].size.load(std::memory_order_relaxed) <= sub_queues_[idx_b].size.load(std::memory_order_relaxed)
          ? idx_a
          : idx_b;
  {
    auto& sq = sub_queues_[idx];
    std::lock_guard guard(sq.lock);
    sq.queue[priority].push_back(operation);
    sq.size.fetch_add(1, std::memory_order_relaxed);
  }
  sema_.signal();
}

void WeakPriorityThreadPool::WorkerThreadLoop(const std::stop_token& stop_token) {
  internal::SetThreadName("weak_pri_worker");
  owning_pool_ = this;

  while (!stop_token.stop_requested()) {
    if (!sema_.wait(static_cast<int64_t>(10) * 1000)) {
      continue;
    }

    auto pop_one = [](SubQueue& sq) -> TypeErasedOperation* {
      auto it = sq.queue.begin();
      auto& fifo = it->second;
      TypeErasedOperation* op = fifo.front();
      fifo.pop_front();
      if (fifo.empty()) {
        sq.queue.erase(it);
      }
      sq.size.fetch_sub(1, std::memory_order_relaxed);
      return op;
    };

    auto peek_priority = [](SubQueue& sq) -> uint32_t { return sq.queue.begin()->first; };

    TypeErasedOperation* operation = nullptr;
    while (operation == nullptr) {
      size_t idx_a = tl_rng() % num_sub_queues_;
      size_t idx_b = tl_rng() % num_sub_queues_;
      if (idx_a == idx_b) {
        idx_b = (idx_b + 1) % num_sub_queues_;
      }

      bool maybe_has_a = sub_queues_[idx_a].size.load(std::memory_order_relaxed) > 0;
      bool maybe_has_b = sub_queues_[idx_b].size.load(std::memory_order_relaxed) > 0;
      if (!maybe_has_a && !maybe_has_b) {
        continue;
      }

      std::scoped_lock guard(sub_queues_[idx_a].lock, sub_queues_[idx_b].lock);

      bool has_a = !sub_queues_[idx_a].queue.empty();
      bool has_b = !sub_queues_[idx_b].queue.empty();
      if (has_a && has_b) {
        if (peek_priority(sub_queues_[idx_a]) <= peek_priority(sub_queues_[idx_b])) {
          operation = pop_one(sub_queues_[idx_a]);
        } else {
          operation = pop_one(sub_queues_[idx_b]);
        }
      } else if (has_a) {
        operation = pop_one(sub_queues_[idx_a]);
      } else if (has_b) {
        operation = pop_one(sub_queues_[idx_b]);
      }
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
