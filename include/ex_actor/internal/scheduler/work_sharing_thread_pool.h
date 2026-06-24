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

#include "ex_actor/internal/alias.h"  // IWYU pragma: keep
#include "ex_actor/internal/container.h"
#include "ex_actor/internal/scheduler/shared/scheduler_operation.h"
#include "ex_actor/internal/scheduler/shared/scheduler_sender.h"

namespace ex_actor {

class WorkSharingThreadPool {
 public:
  using TypeErasedOperation = internal::TypeErasedOperation;

  explicit WorkSharingThreadPool(size_t thread_count, bool start_workers_immediately = true);

  void StartWorkers();

  template <ex::receiver R>
  struct Operation : internal::SchedulerOperationBase<WorkSharingThreadPool, R> {
    using Base = internal::SchedulerOperationBase<WorkSharingThreadPool, R>;
    using Base::Base;

    void start() noexcept { this->thread_pool->EnqueueOperation(this); }
  };

  using Sender = internal::SchedulerSender<WorkSharingThreadPool>;
  using Scheduler = internal::SchedulerHandle<WorkSharingThreadPool>;

  Scheduler GetScheduler() noexcept { return Scheduler {.thread_pool = this}; }

  void EnqueueOperation(TypeErasedOperation* operation);

 private:
  size_t thread_count_;
  internal::UnboundedBlockingQueue<TypeErasedOperation*> queue_;
  std::vector<std::jthread> workers_;
  inline static thread_local TypeErasedOperation* local_slot_ = nullptr;
  inline static thread_local WorkSharingThreadPool* owning_pool_ = nullptr;

  void WorkerThreadLoop(const std::stop_token& stop_token);
};

}  // namespace ex_actor
