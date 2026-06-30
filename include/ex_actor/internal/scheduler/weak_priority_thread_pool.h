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
#include <vector>

#include "ex_actor/3rd_lib/moody_camel_queue/concurrentqueue.h"
#include "ex_actor/3rd_lib/moody_camel_queue/lightweightsemaphore.h"
#include "ex_actor/internal/actor_config.h"
#include "ex_actor/internal/scheduler/shared/scheduler_operation.h"
#include "ex_actor/internal/scheduler/shared/scheduler_sender.h"

namespace ex_actor {

/// A high-performance lock-free priority thread pool, but with the following constraints:
///   1. priority level is limited, must be specified at construction time
///   2. priority is only a hint, not a strict guarantee
class WeakPriorityThreadPool {
 public:
  using TypeErasedOperation = internal::TypeErasedOperation;

  explicit WeakPriorityThreadPool(size_t thread_count, uint32_t max_priority, bool start_workers_immediately = true);

  void StartWorkers();

  template <ex::receiver R>
  struct Operation : internal::SchedulerOperationBase<WeakPriorityThreadPool, R> {
    using Base = internal::SchedulerOperationBase<WeakPriorityThreadPool, R>;
    using Base::Base;

    void start() noexcept {
      auto env = ex::get_env(this->receiver);
      uint32_t priority = ex_actor::get_priority(env);
      this->thread_pool->EnqueueOperation(this, priority);
    }
  };

  using Sender = internal::SchedulerSender<WeakPriorityThreadPool>;
  using Scheduler = internal::SchedulerHandle<WeakPriorityThreadPool>;

  Scheduler GetScheduler() noexcept { return Scheduler {.thread_pool = this}; }

  void EnqueueOperation(TypeErasedOperation* operation, uint32_t priority = 0);

 private:
  size_t thread_count_;
  uint32_t max_priority_;
  std::vector<embedded_3rd::moodycamel::ConcurrentQueue<TypeErasedOperation*>> queues_;
  embedded_3rd::moodycamel::LightweightSemaphore sema_;
  std::vector<std::jthread> workers_;

  struct LocalSlot {
    TypeErasedOperation* op;
    uint32_t priority;
  };
  inline static thread_local LocalSlot local_slot_ = {.op = nullptr, .priority = 0};
  inline static thread_local WeakPriorityThreadPool* owning_pool_ = nullptr;

  void WorkerThreadLoop(const std::stop_token& stop_token);
};

}  // namespace ex_actor
