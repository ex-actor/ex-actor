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

#include <tuple>
#include <utility>
#include <variant>

#include <exec/static_thread_pool.hpp>

#include "ex_actor/internal/actor_config.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/scheduler/stoppable_scheduler_completion_signatures.h"

namespace ex_actor {

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
