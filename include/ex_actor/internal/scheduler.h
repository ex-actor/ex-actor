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
#include <tuple>
#include <utility>
#include <variant>

#include <exec/static_thread_pool.hpp>

#include "ex_actor/internal/actor_config.h"
#include "ex_actor/internal/container.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/platform.h"

namespace ex_actor {

namespace internal {
// Mixin that provides environment-adaptive completion signatures for scheduler senders.
// When the environment has a never_stop_token, advertises only set_value_t(); otherwise
// also advertises set_stopped_t().
struct StoppableSchedulerCompletionSignatures {
  template <class Self>
  static consteval auto get_completion_signatures() {
    return ex::completion_signatures<ex::set_value_t(), ex::set_stopped_t()>();
  }

  template <class Self, class Env>
  static consteval auto get_completion_signatures() {
    if constexpr (ex::unstoppable_token<ex::stop_token_of_t<Env>>) {
      return ex::completion_signatures<ex::set_value_t()>();
    } else {
      return ex::completion_signatures<ex::set_value_t(), ex::set_stopped_t()>();
    }
  }
};
}  // namespace internal

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

  struct TypeErasedOperation {
    virtual ~TypeErasedOperation() = default;
    virtual void Execute() = 0;
  };

  template <ex::receiver R>
  struct Operation : TypeErasedOperation {
    Operation(R receiver, WorkSharingThreadPoolBase* thread_pool)
        : receiver(std::move(receiver)), thread_pool(thread_pool) {}
    R receiver;
    WorkSharingThreadPoolBase* thread_pool;
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

    void start() noexcept {
      uint32_t priority = UINT32_MAX;
      if constexpr (std::is_same_v<Queue<TypeErasedOperation*>,
                                   internal::UnboundedBlockingPriorityQueue<TypeErasedOperation*>>) {
        auto env = ex::get_env(receiver);
        priority = ex_actor::get_priority(env);
      }
      thread_pool->EnqueueOperation(this, priority);
    }
  };

  struct Scheduler;

  struct Sender : ex::sender_t, internal::StoppableSchedulerCompletionSignatures {
    using internal::StoppableSchedulerCompletionSignatures::get_completion_signatures;
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

  void EnqueueOperation(TypeErasedOperation* operation, uint32_t priority = 0) {
    if constexpr (std::is_same_v<Queue<TypeErasedOperation*>,
                                 internal::UnboundedBlockingPriorityQueue<TypeErasedOperation*>>) {
      queue_.Push(operation, priority);
    } else {
      queue_.Push(operation);
    }
  }

 private:
  size_t thread_count_;
  Queue<TypeErasedOperation*> queue_;
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

using WorkSharingThreadPool = WorkSharingThreadPoolBase<internal::UnboundedBlockingQueue>;
using PriorityThreadPool = WorkSharingThreadPoolBase<internal::UnboundedBlockingPriorityQueue>;

class WorkStealingThreadPool : public exec::static_thread_pool {
 public:
  using exec::static_thread_pool::static_thread_pool;
  auto GetScheduler() { return get_scheduler(); }
};

template <class... Schedulers>
class SchedulerUnion {
  static_assert(sizeof...(Schedulers) > 0, "SchedulerUnion must have at least one scheduler");

 public:
  template <class... Args>
    requires(sizeof...(Args) == sizeof...(Schedulers))
  explicit SchedulerUnion(Args&&... schedulers) : schedulers_(std::forward<Args>(schedulers)...) {}

  struct Scheduler;
  struct Sender;
  template <ex::receiver R>
  struct Operation;

  Scheduler GetScheduler() { return Scheduler {.scheduler_union = this}; }

  struct Scheduler : ex::scheduler_t {
    SchedulerUnion* scheduler_union;
    Sender schedule() const noexcept { return {.scheduler_union = scheduler_union}; }
    friend bool operator==(const Scheduler& lhs, const Scheduler& rhs) noexcept {
      return lhs.scheduler_union == rhs.scheduler_union;
    }
  };

  template <ex::receiver R>
  struct Operation {
    using Variant = std::variant<decltype(std::declval<Schedulers&>().schedule().connect(std::declval<R>()))...>;
    Variant inner;

    void start() noexcept {
      std::visit([](auto& inner_op) noexcept { inner_op.start(); }, inner);
    }
  };

  struct Sender : ex::sender_t, internal::StoppableSchedulerCompletionSignatures {
    using internal::StoppableSchedulerCompletionSignatures::get_completion_signatures;
    SchedulerUnion* scheduler_union;

    struct Env {
      SchedulerUnion* scheduler_union;
      template <class CPO>
      auto query(ex::get_completion_scheduler_t<CPO>) const noexcept -> Scheduler {
        return {.scheduler_union = scheduler_union};
      }
    };
    auto get_env() const noexcept -> Env { return Env {.scheduler_union = scheduler_union}; }

    template <ex::receiver R>
    auto connect(R receiver) -> Operation<R> {
      auto env = ex::get_env(receiver);
      size_t scheduler_index = ex_actor::get_scheduler_index(env);
      EXA_THROW_CHECK_LT(scheduler_index, sizeof...(Schedulers)) << "Scheduler index out of range";
      return BuildOperation<R>(std::move(receiver), scheduler_index, std::index_sequence_for<Schedulers...> {});
    }

   private:
    template <ex::receiver R, size_t... kIndices>
    Operation<R> BuildOperation(R receiver, size_t scheduler_index, std::index_sequence<kIndices...>) {
      using Variant = typename Operation<R>::Variant;
      using MakeFn = Variant (*)(SchedulerUnion*, R&&);
      // Dispatch table: one entry per alternative. Each function moves the receiver exactly once.
      static constexpr MakeFn kTable[sizeof...(Schedulers)] = {+[](SchedulerUnion* self, R&& r) -> Variant {
        return Variant {std::in_place_index<kIndices>,
                        std::get<kIndices>(self->schedulers_).schedule().connect(std::move(r))};
      }...};
      return Operation<R> {.inner = kTable[scheduler_index](scheduler_union, std::move(receiver))};
    }
  };

 private:
  std::tuple<Schedulers...> schedulers_;
};

template <class... Schedulers>
SchedulerUnion(Schedulers...) -> SchedulerUnion<Schedulers...>;

}  // namespace ex_actor
