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

#include "ex_actor/internal/platform.h"

namespace ex_actor {

WeakPriorityThreadPool::WeakPriorityThreadPool(size_t thread_count, uint32_t max_priority,
                                               bool start_workers_immediately)
    : thread_count_(thread_count), max_priority_(max_priority > 0 ? max_priority : 1) {
  if (thread_count > 0 && start_workers_immediately) {
    StartWorkers();
  }
}

void WeakPriorityThreadPool::StartWorkers() {
  for (size_t i = 0; i < thread_count_; ++i) {
    workers_.emplace_back([this](const std::stop_token& stop_token) { WorkerThreadLoop(stop_token); });
  }
}

void WeakPriorityThreadPool::EnqueueOperation(TypeErasedOperation* operation, uint32_t priority) {
  if (owning_pool_ == this) {
    if (local_slot_.op == nullptr) {
      local_slot_ = {.op = operation, .priority = priority};
      return;
    }
    if (priority < local_slot_.priority) {
      auto evicted = local_slot_;
      local_slot_ = {.op = operation, .priority = priority};
      operation = evicted.op;
      priority = evicted.priority;
    }
  }
  uint32_t queue_index = priority * kNumQueues / max_priority_;
  if (queue_index >= kNumQueues) queue_index = kNumQueues - 1;
  queues_[queue_index].enqueue(operation);
  sema_.signal();
}

void WeakPriorityThreadPool::WorkerThreadLoop(const std::stop_token& stop_token) {
  internal::SetThreadName("weak_pri_worker");
  owning_pool_ = this;
  while (!stop_token.stop_requested()) {
    if (!sema_.wait(static_cast<int64_t>(10) * 1000)) {
      continue;
    }
    TypeErasedOperation* operation = nullptr;
    for (auto& queue : queues_) {
      if (queue.try_dequeue(operation)) {
        break;
      }
    }
    if (operation == nullptr) {
      sema_.signal();
      continue;
    }
    operation->Execute();
    while (local_slot_.op != nullptr) {
      operation = local_slot_.op;
      local_slot_.op = nullptr;
      operation->Execute();
    }
  }
  owning_pool_ = nullptr;
}

}  // namespace ex_actor
