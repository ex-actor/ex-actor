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

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "ex_actor/3rd_lib/moody_camel_queue/concurrentqueue.h"  // IWYU pragma: keep
#include "ex_actor/3rd_lib/moody_camel_queue/lightweightsemaphore.h"
#include "ex_actor/internal/actor_config.h"
#include "ex_actor/internal/scheduler/shared/scheduler_operation.h"
#include "ex_actor/internal/scheduler/shared/scheduler_sender.h"

namespace ex_actor {

/// A sharded thread pool with best-effort (non-strict) priority, using "power of two choices":
///   - Push: randomly pick 2 sub-queues, enqueue to the less loaded one
///   - Pop: randomly pick 2 sub-queues, peek their tops, take the higher-priority item
class WeakPriorityThreadPool {
 public:
  using TypeErasedOperation = internal::TypeErasedOperation;

  explicit WeakPriorityThreadPool(size_t thread_count, size_t num_sub_queues = 0);

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
  struct alignas(128) SubQueue {
    std::mutex lock;
    std::map<uint32_t, std::deque<TypeErasedOperation*>> queue;
    std::atomic<size_t> size {0};
  };

  size_t thread_count_;
  size_t num_sub_queues_;
  std::vector<SubQueue> sub_queues_;
  embedded_3rd::moodycamel::LightweightSemaphore sema_;
  std::vector<std::jthread> workers_;
  inline static thread_local TypeErasedOperation* local_slot_ = nullptr;
  inline static thread_local uint32_t local_slot_priority_ = 0;
  inline static thread_local WeakPriorityThreadPool* owning_pool_ = nullptr;

  void WorkerThreadLoop(const std::stop_token& stop_token);
};

}  // namespace ex_actor
