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

#include <thread>
#include <utility>

#include "ex_actor/internal/alias.h"  // IWYU pragma: keep
#include "ex_actor/internal/container.h"
#include "ex_actor/internal/platform.h"
#include "ex_actor/internal/scheduler/stoppable_scheduler_completion_signatures.h"

namespace ex_actor {

class WorkSharingThreadPool {
 public:
  explicit WorkSharingThreadPool(size_t thread_count, bool start_workers_immediately = true)
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

  struct TypeErasedOperation {
    virtual ~TypeErasedOperation() = default;
    virtual void Execute() = 0;
  };

  template <ex::receiver R>
  struct Operation : TypeErasedOperation {
    Operation(R receiver, WorkSharingThreadPool* thread_pool)
        : receiver(std::move(receiver)), thread_pool(thread_pool) {}
    R receiver;
    WorkSharingThreadPool* thread_pool;
    void Execute() override {
      auto env = ex::get_env(receiver);
      auto stoken = ex::get_stop_token(env);
      if constexpr (ex::unstoppable_token<decltype(stoken)>) {
        receiver.set_value();
      } else {
        if (stoken.stop_requested()) {
          receiver.set_stopped();
        } else {
          receiver.set_value();
        }
      }
    }

    void start() noexcept { thread_pool->EnqueueOperation(this); }
  };

  struct Scheduler;

  struct Sender : ex::sender_t, internal::StoppableSchedulerCompletionSignatures {
    using internal::StoppableSchedulerCompletionSignatures::get_completion_signatures;
    WorkSharingThreadPool* thread_pool;

    struct Env {
      WorkSharingThreadPool* thread_pool;
      template <class CPO>
      auto query(ex::get_completion_scheduler_t<CPO>) const noexcept -> Scheduler {
        return {.thread_pool = thread_pool};
      }
    };
    auto get_env() const noexcept -> Env { return Env {.thread_pool = thread_pool}; }
    template <ex::receiver R>
    Operation<R> connect(R receiver) {
      return {std::move(receiver), thread_pool};
    }
  };

  struct Scheduler : ex::scheduler_t {
    WorkSharingThreadPool* thread_pool;
    Sender schedule() const noexcept { return {.thread_pool = thread_pool}; }
    friend bool operator==(const Scheduler& lhs, const Scheduler& rhs) noexcept {
      return lhs.thread_pool == rhs.thread_pool;
    }
  };

  Scheduler GetScheduler() noexcept { return Scheduler {.thread_pool = this}; }

  void EnqueueOperation(TypeErasedOperation* operation) { queue_.Push(operation); }

 private:
  size_t thread_count_;
  internal::UnboundedBlockingQueue<TypeErasedOperation*> queue_;
  std::vector<std::jthread> workers_;

  void WorkerThreadLoop(const std::stop_token& stop_token) {
    internal::SetThreadName("ws_pool_worker");
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
