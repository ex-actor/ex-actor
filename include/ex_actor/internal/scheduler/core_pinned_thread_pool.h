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

#include <thread>

#include "ex_actor/internal/actor_config.h"
#include "ex_actor/internal/container.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/platform.h"
#include "ex_actor/internal/scheduler/scheduler_operation.h"
#include "ex_actor/internal/scheduler/scheduler_sender.h"

namespace ex_actor {

class CorePinnedThreadPool {
 public:
  using TypeErasedOperation = internal::TypeErasedOperation;

  explicit CorePinnedThreadPool(size_t thread_count) : thread_count_(thread_count), queues_(thread_count) {
    EXA_THROW_CHECK(thread_count > 0) << "Thread count must be greater than 0";
    for (size_t i = 0; i < thread_count; ++i) {
      workers_.emplace_back([this, i](const std::stop_token& stop_token) { WorkerThreadLoop(stop_token, i); });
    }
  }

  template <ex::receiver R>
  struct Operation : internal::SchedulerOperationBase<CorePinnedThreadPool, R> {
    using Base = internal::SchedulerOperationBase<CorePinnedThreadPool, R>;
    using Base::Base;

    void start() noexcept {
      auto env = ex::get_env(this->receiver);
      size_t core_index = ex_actor::get_core_index(env);
      this->thread_pool->EnqueueOperation(this, core_index);
    }
  };

  using Sender = internal::SchedulerSender<CorePinnedThreadPool>;
  using Scheduler = internal::SchedulerHandle<CorePinnedThreadPool>;

  Scheduler GetScheduler() noexcept { return Scheduler {.thread_pool = this}; }

  void EnqueueOperation(TypeErasedOperation* operation, size_t core_index) {
    size_t queue_idx = core_index % thread_count_;
    queues_[queue_idx].Push(operation);
  }

 private:
  size_t thread_count_;
  std::vector<internal::UnboundedBlockingQueue<TypeErasedOperation*>> queues_;
  std::vector<std::jthread> workers_;

  void WorkerThreadLoop(const std::stop_token& stop_token, size_t thread_index) {
    internal::SetThreadName(fmt_lib::format("cp_pool_worker_{}", thread_index));
    internal::SetThreadAffinity(thread_index);

    while (!stop_token.stop_requested()) {
      auto optional_operation = queues_[thread_index].Pop(/*timeout_ms=*/10);
      if (!optional_operation) {
        continue;
      }
      optional_operation.value()->Execute();
    }
  }
};

}  // namespace ex_actor
