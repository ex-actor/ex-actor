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

#pragma once

#include <cstdint>
#include <thread>

#include "ex_actor/internal/actor_config.h"
#include "ex_actor/internal/container.h"
#include "ex_actor/internal/platform.h"
#include "ex_actor/internal/scheduler/scheduler_operation.h"
#include "ex_actor/internal/scheduler/scheduler_sender.h"

namespace ex_actor {

class PriorityThreadPool {
 public:
  using TypeErasedOperation = internal::TypeErasedOperation;

  explicit PriorityThreadPool(size_t thread_count, bool start_workers_immediately = true)
      : thread_count_(thread_count) {
    if (thread_count > 0 && start_workers_immediately) {
      StartWorkers();
    }
  }

  void StartWorkers() {
    for (size_t i = 0; i < thread_count_; ++i) {
      workers_.emplace_back([this](const std::stop_token& stop_token) { WorkerThreadLoop(stop_token); });
    }
  }

  template <ex::receiver R>
  struct Operation : internal::SchedulerOperationBase<PriorityThreadPool, R> {
    using Base = internal::SchedulerOperationBase<PriorityThreadPool, R>;
    using Base::Base;

    void start() noexcept {
      auto env = ex::get_env(this->receiver);
      uint32_t priority = ex_actor::get_priority(env);
      this->thread_pool->EnqueueOperation(this, priority);
    }
  };

  using Sender = internal::SchedulerSender<PriorityThreadPool>;
  using Scheduler = internal::SchedulerHandle<PriorityThreadPool>;

  Scheduler GetScheduler() noexcept { return Scheduler {.thread_pool = this}; }

  void EnqueueOperation(TypeErasedOperation* operation, uint32_t priority = 0) { queue_.Push(operation, priority); }

 private:
  size_t thread_count_;
  internal::UnboundedBlockingPriorityQueue<TypeErasedOperation*> queue_;
  std::vector<std::jthread> workers_;

  void WorkerThreadLoop(const std::stop_token& stop_token) {
    internal::SetThreadName("priority_pool_worker");
    while (!stop_token.stop_requested()) {
      auto optional_operation = queue_.Pop(/*timeout_ms=*/10);
      if (!optional_operation) {
        continue;
      }
      optional_operation.value()->Execute();
    }
  }
};

}  // namespace ex_actor
