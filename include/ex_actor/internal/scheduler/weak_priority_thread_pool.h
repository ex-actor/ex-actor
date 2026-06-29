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
#include "ex_actor/internal/scheduler/shared/scheduler_operation.h"
#include "ex_actor/internal/scheduler/shared/scheduler_sender.h"

namespace ex_actor {

// Thread pool with priority-aware local scheduling. The shared queue is a simple FIFO
// (single-bucket) for minimal contention. Priority ordering is achieved locally: the
// thread-local slot holds the highest-priority pending operation, evicting lower-priority
// items to the shared queue. This gives FIFO-queue speed (no bitmap/bucket scanning) while
// preserving critical-path-first execution for coarse-grained graphs.
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
  static constexpr uint32_t kNumQueues = 8;

  size_t thread_count_;
  uint32_t max_priority_;
  embedded_3rd::moodycamel::ConcurrentQueue<TypeErasedOperation*> queues_[kNumQueues];
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
