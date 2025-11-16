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

#include <exec/static_thread_pool.hpp>

#include "ex_actor/3rd_lib/moody_camel_queue/blockingconcurrentqueue.h"
#include "ex_actor/internal/actor_config.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/util.h"

namespace ex_actor {

template <template <class> class Queue>
class WorkSharingThreadPoolBase {
 public:
  explicit WorkSharingThreadPoolBase(size_t thread_count, bool start_workers_immediately = true)
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

  struct TypeEasedOperation {
    virtual ~TypeEasedOperation() = default;
    virtual void Execute() = 0;
  };

  template <ex::receiver R>
  struct Operation : TypeEasedOperation {
    Operation(R receiver, WorkSharingThreadPoolBase* thread_pool)
        : receiver(std::move(receiver)), thread_pool(thread_pool) {}
    R receiver;
    WorkSharingThreadPoolBase* thread_pool;
    void Execute() override {
      auto env = stdexec::get_env(receiver);
      auto stoken = stdexec::get_stop_token(env);
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

    void start() noexcept {
      uint32_t priority = UINT32_MAX;
      if constexpr (std::is_same_v<Queue<TypeEasedOperation*>,
                                   internal::util::UnboundedBlockingPriorityQueue<TypeEasedOperation*>>) {
        auto env = stdexec::get_env(receiver);
        priority = ex_actor::get_priority(env);
      }
      thread_pool->EnqueueOperation(this, priority);
    }
  };

  struct Scheduler;

  struct Sender : ex::sender_t {
    using completion_signatures = ex::completion_signatures<ex::set_value_t(), ex::set_stopped_t()>;
    WorkSharingThreadPoolBase* thread_pool;
    struct Env {
      WorkSharingThreadPoolBase* thread_pool;
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
    WorkSharingThreadPoolBase* thread_pool;
    Sender schedule() const noexcept { return {.thread_pool = thread_pool}; }
    friend bool operator==(const Scheduler& lhs, const Scheduler& rhs) noexcept {
      return lhs.thread_pool == rhs.thread_pool;
    }
  };

  Scheduler GetScheduler() noexcept { return Scheduler {.thread_pool = this}; }

  void EnqueueOperation(TypeEasedOperation* operation, uint32_t priority = 0) {
    if constexpr (std::is_same_v<Queue<TypeEasedOperation*>,
                                 internal::util::UnboundedBlockingPriorityQueue<TypeEasedOperation*>>) {
      queue_.Push(operation, priority);
    } else {
      queue_.Push(operation);
    }
  }

 private:
  size_t thread_count_;
  Queue<TypeEasedOperation*> queue_;
  std::vector<std::jthread> workers_;

  void WorkerThreadLoop(const std::stop_token& stop_token) {
    internal::util::SetThreadName("ws_pool_worker");
    while (!stop_token.stop_requested()) {
      auto optional_operation = queue_.Pop(/*timeout_ms=*/10);
      if (!optional_operation) {
        continue;
      }
      optional_operation.value()->Execute();
    }
  }
};

using WorkSharingThreadPool = WorkSharingThreadPoolBase<internal::util::UnboundedBlockingQueue>;
using PriorityThreadPool = WorkSharingThreadPoolBase<internal::util::UnboundedBlockingPriorityQueue>;

class WorkStealingThreadPool : public exec::static_thread_pool {
 public:
  using exec::static_thread_pool::static_thread_pool;
  auto GetScheduler() { return get_scheduler(); }
};

template <class InnerScheduler>
class SchedulerUnion {
 public:
  explicit SchedulerUnion(std::vector<InnerScheduler> schedulers, size_t default_scheduler_index = 0)
      : schedulers_(std::move(schedulers)), default_scheduler_index_(default_scheduler_index) {
    EXA_THROW_CHECK_GT(schedulers_.size(), 0) << "SchedulerUnion must have at least one scheduler";
  }

  class Scheduler;
  class Sender;
  template <ex::receiver R>
  class Operation;

  Scheduler GetScheduler() { return Scheduler {.scheduler_union = this}; }

  struct Scheduler : ex::scheduler_t {
    SchedulerUnion* scheduler_union;
    Sender schedule() const noexcept { return {.scheduler_union = scheduler_union}; }
    friend bool operator==(const Scheduler& lhs, const Scheduler& rhs) noexcept {
      return lhs.scheduler_union == rhs.scheduler_union;
    }
  };

  struct Sender : ex::sender_t {
    using completion_signatures = ex::completion_signatures<ex::set_value_t(), ex::set_stopped_t()>;
    SchedulerUnion* scheduler_union;
    struct Env {
      SchedulerUnion* scheduler_union;
      template <class CPO>
      auto query(ex::get_completion_scheduler_t<CPO>) const noexcept -> Scheduler {
        return {.thread_pool = scheduler_union};
      }
    };
    auto get_env() const noexcept -> Env { return Env {.thread_pool = scheduler_union}; }

    auto connect(ex::receiver auto receiver) {
      auto env = stdexec::get_env(receiver);
      auto scheduler_index = ex_actor::get_scheduler_index(env);
      EXA_THROW_CHECK_LT(scheduler_index, scheduler_union->schedulers_.size()) << "Scheduler index out of range";
      return scheduler_union->schedulers_[scheduler_index].schedule().connect(std::move(receiver));
    }
  };

 private:
  std::vector<InnerScheduler> schedulers_;
  size_t default_scheduler_index_;
};
}  // namespace ex_actor